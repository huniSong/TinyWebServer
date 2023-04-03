#ifndef LOG_H
#define LPG_H

#include <stdio.h>  // 提供fputs函数
#include <string>
#include <stdio.h>
#include "block_queue.h"
#include "locker.h"

using namespace std;

// 使用单例模式创建日志系统
class Log {
public:
    // 使用共有的静态方法获得实例，c++11后使用局部变量懒汉不用加锁
    static Log* get_instance() {
        static Log instance;
        return &instance;
    }

    // 异步写日志公共方法，调用私有方法async_write_log
    static void* flush_log_thread(void* args) {
        Log::get_instance()->async_write_log();
    }

    // 日志参数包括日志文件、日志缓冲区大小、最大行数以及最长日志条队列（若队列大小为0，则为同步，否则为异步）
    bool init(const char* file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    // 将输出内容按标准格式整理
    void write_log(int level, const char* format, ...);

    // 强制刷新缓冲区
    void flush(void);  

private:
    Log();  // 私有化构造函数，以防外界创建单例类的对象
    virtual ~Log();  // 虚析构函数：当父类指针指向子类对象的时候，把父类的析构函数设置成虚析构，防止内存泄露

    // 异步写日志
    void* async_write_log() {
        string single_log;
        // 从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log)) {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);  // 将single_log写入m_fp所指向的文件
            m_mutex.unlock();
        }
    }

private:
    block_queue<string>* m_log_queue;  // 阻塞队列
    locker m_mutex;  // 互斥锁
    FILE* m_fp;  // log文件指针

    char dir_name[128];  // 路径名
    char log_name[128];  // log文件名
    int m_log_buf_size;  // 日志缓冲区的大小
    int m_split_lines;  // 日志最大行数
    long long m_count;  // 日志行数记录
    int m_today;  // 按天分文件，记录当前时间是哪一天

    bool m_is_async;  // 是否异步标志位
    char* m_buf;  // 要输出的内容


};

// 日志分级，以下宏定义在其他文件中使用，主要用于不同类型的日志输出（Warn实际并未使用）
// Debug：调试代码的输出，在系统实际运行时一般不使用
#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__);
// Info：报告系统当前的状态，当前执行的流程或接收的信息
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__);
// Warn：与调试时中断的warning类似，调试代码时使用
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__);
// Error：输出系统的错误
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__);

#endif