#include <pthread.h>
#include <string.h>  // 提供memest函数
#include <sys/time.h>  // 提供gettimeofday函数
#include <stdarg.h>  // 提供va_start函数
#include "log.h"

Log::Log() {
    m_count = 0;
    m_is_async = false;
}

Log::~Log() {
    if (m_fp != NULL) fclose(m_fp);
}


// 调用init方法，初始化生成日志文件，服务器启动按当前时刻创建日志，前缀为时间，
// 后缀为自定义log文件名，并记录创建日志的时间day和行数count。异步需要设置阻塞队列的长度，同步不需要
bool Log::init(const char* file_name, int log_buf_size, int split_lines, int max_queue_size) {
    // 如果max_queue_size不为0则为异步
    if (max_queue_size >= 1) {
        m_is_async = true;

        // 创建并设置阻塞队列长度
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;

        // flush_log_thread为回调函数，此处表示创建线程异步写数据
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    // 缓冲区设置
    m_log_buf_size= log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', sizeof(m_buf));  // 将m_buf数组元素全部初始化为0

    // 日志最大长度
    m_split_lines = split_lines;

    // 获取当前时间
    time_t t = time(NULL);  
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    
    // 从后往前找到一个'/'的位置，而strchr是从前往后
    const char* p = strrchr(file_name, '/');
    char log_full_name[255] = {0};
    // 自定义日志名
    if (p == NULL) {
        // 若输入的文件名没有'/'，则直接将时间+文件名作为日志名
        // snprintf将可变个参数(...)按照%d_%02d_%02d_%s格式化成字符串（02的意思是如果输出的整型数不足两位，左侧用0补齐），然后将其复制到log_full_name中。
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } else {
        // 将p向后移动一个位置，然后复制到log_name中
        strcpy(log_name, p + 1);
        // 将路径赋值到dir_name
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_02%d_02%d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");  // 以追加的方式打开只写文件。若文件不存在，则会建立该文件，如果文件存在，写入的数据会被加到文件尾，即文件原先的内容会被保留
    if (m_fp == NULL) return false;
    return true;

}

// 工作线程写日志
void Log::write_log(int level, const char* format, ...) {
    
    // 获取时间
    struct timeval now = {0, 0};  // now有两个成员，秒数和毫秒数
    gettimeofday(&now, NULL);  // 返回自1970-01-01 00:00:00到现在经历的秒数
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    
    char s[16] = {0};
    // 日志分级
    switch (level) {
        case 0: {
            strcpy(s, "[debug]:");
            break;
        }
        case 1: {
            strcpy(s, "[info]:");
            break;
        }
        case 2: {
            strcpy(s, "[warn]:");
            break;
        }
        case 3: {
            strcpy(s, "[erro]:");
            break;
        }
        default: {
            strcpy(s, "[info]:");
            break;
        }
    }
    
    m_mutex.lock();
    m_count++;  // 更新行数
    // 日志不是今天，或写入的日志行数是最大行的倍数
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {
        char new_log[258] = {0};
        /*在使用多个输出函数连续进行多次输出到控制台时，有可能下一个数据再上一个数据还没输出完毕，
        还在输出缓冲区中时，下一个printf就把另一个数据加入输出缓冲区，结果冲掉了原来的数据，出
        现输出错误。fflush会强迫将缓冲区内的数据写回参数m_fp指定的文件中*/
        fflush(m_fp);
        fclose(m_fp);

        // 重新定义日志名中的时间部分
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        if (m_today != my_tm.tm_mday) {  // 如果时间不是今天，则创建今天的日志，更新m_today和m_count
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {  // 超过了最大行，在之前的日志名后加m_count / m_split_lines
            snprintf(new_log, 257, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    // 定义可变参数列表
    va_list valst;
    // 将本函数传入的参数format赋值给valst，便于格式化输出
    va_start(valst, format);
    
    string log_str;
    m_mutex.lock();
    // 写入内容格式：时间+内容
    // 时间格式化，snprintf返回写字符的总数（不包括结尾的NULL）
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s", my_tm.tm_year, my_tm.tm_mon, my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    // 内容格式化，用于向字符串中打印数据，并返回写字符总数
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;
    m_mutex.unlock();

    // 若m_is_async为true表示异步写，需要将日志信息加入阻塞队列，否则为同步直接加锁向文件中写
    if (m_is_async && !m_log_queue->full()) m_log_queue->push(log_str);
    else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);  // 清空可变参数列表

}

void Log::flush() {
    m_mutex.lock();
    fflush(m_fp);  // 强制刷新写入缓冲流
    m_mutex.unlock();
}