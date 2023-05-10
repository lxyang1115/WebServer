#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>

// 线程池类，模板类，为了代码复用
template <typename T>
class threadpool {
public:
    threadpool(int thread_num = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);
    void run();

private:
    int m_thread_num;  // 数量
    pthread_t * m_threads;  // 线程池数组
    int m_max_requests;  // 请求队列中最多允许的请求数量
    std::list<T*> m_workqueue;  // 请求队列
    locker m_queuelocker;  // 互斥锁
    sem m_queuestat;  // 信号量用于判断是否有任务需要处理
    bool m_stop;  // 是否结束线程

    static void *worker(void *arg);
};

template <typename T>
threadpool<T>::threadpool(int thread_num, int max_requests) : m_thread_num(thread_num), m_max_requests(max_requests), m_stop(false), m_threads(NULL) {
    if (thread_num <= 0 || max_requests <= 0) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_num];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建线程，设置为线程脱离（自己释放资源）
    for (int i = 0; i < thread_num; i++) {
        printf("create the %dth thread\n", i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {  // worker:静态函数
            delete [] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]) != 0) {
            delete [] m_threads;
            throw std::exception();
        } 
    }

}

template <typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request) {
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();

    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg) {
    threadpool *pool = (threadpool *) arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }

        request->process();
    }
}

# endif