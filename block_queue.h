#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include "locker.h"

template <class T>
class block_queue {
public:
    // 初始化私有资源
    block_queue(int max_size = 1000) {
        if (max_size <= 0) exit(-1);
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    // 为了保证线程安全，每个操作前都需要加锁，操作结束后再解锁

    // 析构函数，释放数组资源
    ~block_queue() {
        m_mutex.lock();
        if (m_array != NULL) delete[] m_array;
        m_mutex.unlock();
    }

    // 清空阻塞队列
    void clear() {
        m_mutex.lock();
        m_size = 0;
        m_front - -1;
        m_back = -1;
        m_mutex.unlock();
    }

    // 判断队列是否已满
    bool full() {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            m_mutex.unlock();
            return false;
        }
        m_mutex.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty() {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 返回队首元素
    bool front(T& value) {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T& value) {
        m_mutex.lock();
        if (0 == m_size) {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];  // value是传出参数
        m_mutex.unlock();
        return true;
    }

    // 获取阻塞队列长度
    int size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    // 获取阻塞队列最大长度
    int max_size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    // 往队列添加元素，需要将所有使用队列的线程先唤醒，当前有元素push进队列，相当于生产者产生了一个元素
    bool push(const T& item) {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            m_cond.broadcast();  // 如果队列已满则唤醒所有等待该条件变量的线程
            m_mutex.unlock();
            return false;
        }
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;
        m_cond.broadcast();
        m_mutex.lock();
        return true;
    }

    // 从队列中取元素，如果当前队列没有元素，则会等待条件变量
    bool pop(T& item) {
        m_mutex.lock();
        // 当有多个消费者时，用while而非if
        while (m_size <= 0) {
            if (!m_cond.wait(m_mutex.get())) {
                m_mutex.unlock();
                return false;
            }
        }
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];  // 传出参数
        m_size--;
        m_mutex.lock();
        return true;
    }
private:
    locker m_mutex;  // 互斥锁
    cond m_cond;  // 条件变量

    T* m_array;  // 循环数组
    int m_size;  // 目前数组长度
    int m_max_size;  // 数组最大长度
    int m_front;
    int m_back;

};


#endif