#ifndef UTILTIMER_H
#define UTILTIMER_H

#include <time.h>

class http_conn;

class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

    time_t expire; // 任务超时时间，这里使用绝对时间

    http_conn* user_conn; 

    util_timer* prev;    // 指向前一个定时器
    util_timer* next;    // 指向后一个定时器
};

// 定时器链表，它是一个升序、双向链表，且带有头节点和尾节点。
class sort_timer_list {
public:
    sort_timer_list();
    ~sort_timer_list();

    void add_timer(util_timer* t);

    void adjust_timer(util_timer* t);

    void del_timer(util_timer* t);

    /* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
    void tick();

private:
    util_timer* head;
    util_timer* tail;

    void add_timer_from(util_timer* t, util_timer* f);
};


#endif