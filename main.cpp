#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>

//#define LOG

#include "log.h"
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5 //生成一次alarm信号的间隔

// 定时器相关变量
static int pipefd[2];  // 套接字柄对，pipefd[0]用于读，pipefd[1]用于写
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 引入定义在其他文件中的外部函数
// 设置文件描述符非阻塞
extern void setnonblocking(int fd);
// 添加文件描述符到epoll
extern int addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除文件描述符
extern int removefd(int epollfd, int fd);
// 修改文件描述符到epoll
extern int modfd(int epollfd, int fd, int ev);

// 添加信号捕捉
void addsig(int sig, void(handler)(int), bool restart = true) {
    struct sigaction sa;  // sa是注册信号的参数
    memset(&sa, '\0', sizeof(sa));  // sa置为0

    sa.sa_handler = handler;  // 函数指针，指向信号捕捉到之后的处理函数
    if (restart) sa.sa_flags |= SA_RESTART;  // 信号返回时重新启动系统调用
    sigfillset(&sa.sa_mask);  // 将信号集所有标志位置为1，即临时都是阻塞的，该函数的参数是传出参数即赋值给sa.sa_mask
    
    assert(sigaction(sig, &sa, NULL) != -1);  // 功能是检查或者改变信号的处理
}

// 信号处理函数，仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响
void sig_handler(int sig) {

    // 为了保证函数的可重入性（可重入性表示中断后再次进入该函数环境变量与之前相同，不会丢失数据），保留errno
    int save_error = errno;
    int msg = sig;

    // 将信号值从管道写端写入
    send(pipefd[1], (char*)&msg, 1, 0);  // 当套接字发送缓冲区变满时，send通常会阻塞，返回EAGAIN或EWOULDBLOCK错误，所以将pipefd[1]改成非阻塞

    // 将原来的errno赋值给当前的errno
    errno = save_error;
}

// 定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data* user_data) {
    // 删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    // 减少连接数目
    http_conn::m_user_count--;

    // 关闭文件描述符
    close(user_data->sockfd);

#ifdef LOG
    // 写日志
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
#endif
}

// 定时处理任务并重新定时以不断触发SIGALRM信号
void timer_handler() {
    timer_lst.tick();
    alarm(SIGALRM);
}

void show_error(int connfd, const char* info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 主函数入口
int main( int argc, char* argv[] ) {

#ifdef LOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);  // 异步日志，最后一个参数是队列大小
#endif

    if (argc <= 1) {
        printf("按照如下命令执行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    int port = atoi( argv[1] ); // ascii to int
    
    // 对SIGPIPE信号进行处理（如果通信双方一端关闭，另一端还在写数据，则会收到SIGPIPE信号）
    addsig(SIGPIPE, SIG_IGN);  // 由于SIGPIPE默认会终止程序，所以设置SIG_IGN忽略该信号

    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        exit(-1);
    }

    // 创建一个数组，用于保存所有的客户端信息
    http_conn* users = new http_conn[MAX_FD];
    assert(users);

    // 创建监听套接字，使用IPv4（PF_INET）协议族，流式协议（SOCK_STREAM），第三个参数一般写0， 流式协议默认使用TCP，报式协议默认使用UDP
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);  // 创建成功则返回文件描述符，失败则返回-1
    assert(listenfd >= 0);  // assert中条件表达式如果不满足的话，会返回错误并终止程序

    int ret = 0;
    struct sockaddr_in address; //sockaddr_in用于IPv4，sockaddr_in6用于IPv6
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 设置端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //绑定端口，开始监听
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 ); // 创建一个指示epoll内核事件表的文件描述符，5没有意义，只要大于0即可，失败返回-1，成功返回epoll的文件描述符

    // 将监听描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    // 定时器事件相关
    // 创建管道套接字，其中管道写端写入信号值，管道读端通过I/O复用系统监测读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);  // 第一个参数表示协议族，第二个参数表示协议，第三个参数表示类型，只能为0，第四个参数是套接字柄对
    assert(ret != -1);
    // 设置管道写端为非阻塞
    setnonblocking(pipefd[1]);
    // 统一事件源，信号处理函数使用管道将信号传递给主循环，
    // 信号处理函数往管道的写端写入信号值，主循环则从管道的读端读出信号值
    addfd(epollfd, pipefd[0], false);
    // 传递给主循环的信号值，添加信号捕捉
    addsig(SIGALRM, sig_handler, false);  // 由alarm系统调用产生timer时钟信号
    addsig(SIGTERM, sig_handler, false);  // 终端发送的终止信号
    // 每隔TIMESLOT时间触发SIGALRM
    alarm(TIMESLOT);  // 设置信号SIGALRM在经过TIMESLOT秒后发送给目前的进程
    bool timeout = false;  // 超时标志
    bool stop_server = false;  // 循环条件
    // 在堆区创建用户的连接资源数组，记录连接和定时器
    client_data* users_timer = new client_data[MAX_FD];

    while (!stop_server) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ((number < 0) && (errno != EINTR)) {
            printf( "epoll failure\n" );
#ifdef LOG
            LOG_ERROR("%s", "epoll failure");
#endif
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < number; i++) {
            
            int sockfd = events[i].data.fd;
            
            // 如果事件发生在监听文件描述符上
            if (sockfd == listenfd) {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                // 分配给客户端的文件描述符           
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);  // client_address是传出参数，成功则返回用于通信的文件描述符，失败返回-1
                if (connfd < 0) {
#ifdef LOG
                    // 日志
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
#endif
                    continue;
                }

                if (http_conn::m_user_count >= MAX_FD) {
                    // 目前连接已满，给客户端一个信息，显示服务器正忙
                    show_error(connfd, "Internal sever busy");
#ifdef LOG
                    // 日志
                    LOG_ERROR("%s", "Internal server busy");
#endif
                    continue;
                }            
                // 将新的客户数据初始化放入用户数组中
                users[connfd].init(connfd, client_address);

                // 初始化新的客户的连接资源
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                // 创建定时器，并设置连接资源、回调函数、超时时间
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);  // 当前时间
                timer->expire = cur + 3 * TIMESLOT;
                // 连接资源绑定定时器
                users_timer[connfd].timer = timer;
                // 将定时器添加到双向链表中
                timer_lst.add_timer(timer);

            } else if (events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或者出现错误等事件
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]); //关闭连接
                if (timer) timer_lst.del_timer(timer); //删除定时器

            } else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                // 处理信号
                char signals[1024];
                // 管道读端对应的文件描述符发生读事件，成功返回字节数，失败返回-1
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) continue;
                else if (ret == 0) continue;
                else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            case SIGALRM: {
                                // 定时任务不立即处理，先在这里标记，优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM: {
                                // 检测到终止信号
                                stop_server = true;
                            }
                        }
                    }
                }
            } else if (events[i].events & EPOLLIN) {

                // 创建定时器临时变量，将该连接对应的定时器取出
                util_timer* timer = users_timer[sockfd].timer;

                if (users[sockfd].read()) {  // 一次性把所有数据都读完
#ifdef LOG
                    // 日志
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
#endif
                    // 若监听到读事件，则将该事件放入请求队列
                    
                    
                    pool->append(users + sockfd);

                    // 若有数据传输，则更新定时器，延迟3个单位，并调整定时器在链表中的位置
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
#ifdef LOG
                        // 日志
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
#endif
                    }
                }

            } else if (events[i].events & EPOLLOUT) {
                util_timer* timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {  // 一次性写完所有数据
#ifdef LOG
                    // 日志
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
#endif
                    // 若有数据传输，则更新定时器，延时3个SLOT，并调整定时器在链表中的位置
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
#ifdef LOG
                        // 日志
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
#endif
                    }
                } else {  
                    // 否则移除链表上的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) timer_lst.del_timer(timer);
                }
            }
        }
        // 处理定时器
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }//end while

    // 释放资源
    close(epollfd);  // 关闭指示epoll内核事件表的文件描述符
    close(listenfd);  // 关闭监听的文件描述符
    close(pipefd[0]);  // 关闭读端文件描述符
    close(pipefd[1]);  // 关闭写端文件描述符
    delete[] users;  // 删除用户数组
    delete[] users_timer;  // 删除用户连接资源数组
    delete pool;  // 删除线程池

    return 0;
}