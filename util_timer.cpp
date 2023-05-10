#include "util_timer.h"
#include "http_conn.h"

sort_timer_list::sort_timer_list() : head(NULL), tail(NULL) {}
sort_timer_list::~sort_timer_list() {
    util_timer* tmp = head;
    while (tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_list::add_timer(util_timer* t) {
    // printf("add\n");

    if (!t) return;
    if (!head) {
        head = tail = t;
        return;
    }

    if (t->expire < head->expire) {
        t->next = head;
        head->prev = t;
        head = t;
        return;
    }

    add_timer_from(t, head);

}

void sort_timer_list::adjust_timer(util_timer* t) {
    // printf("adjust %d %d %d\n", t, head, tail);
    // printf("%d %d %d\n", t->next, head->prev, tail);
    if (!t) return;
    if (t == tail) return; // 尾部
    util_timer* tmp = t->next;
    if (t->expire <= tmp->expire) return;

    if (t == head) {
        head = t->next;
        head->prev = NULL;
        add_timer_from(t, head);
    } else {
        t->next->prev = t->prev;
        t->prev->next = t->next;
        add_timer_from(t, t->next);
    }
}

void sort_timer_list::del_timer(util_timer* t) {
    if (!t) return;
    // printf("%d %d %d\n", t->next, head->prev, tail);
    if (t == head && t == tail) {
        delete t;
        head = tail = NULL;
        return;
    }

    if (t == head) {
        head = t->next;
        head->prev = NULL;
        delete t;
        return;
    }

    if (t == tail) {
        tail = t->prev;
        tail->next = NULL;
        delete t;
        return;
    }

    t->prev->next = t->next;
    t->next->prev = t->prev;
    delete t;
}

/* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
void sort_timer_list::tick() {
    if (!head) return;
    // printf("timer tick\n");
    time_t cur = time(NULL); // 当前系统时间
    util_timer* tmp = head;
    while (tmp) {
        if (cur < tmp->expire) break;
        // 关闭连接，同时删除定时器
        tmp->user_conn->close_conn();
        tmp = head;
    }
}

void sort_timer_list::add_timer_from(util_timer* t, util_timer* f) {
    util_timer *p = f, *tmp = f->next;
    while (tmp) {
        if (t->expire < tmp->expire) {
            p->next = t;
            t->prev = p;
            tmp->prev = t;
            t->next = tmp;
            return;
        }
        p = tmp;
        tmp = tmp->next;
    }

    if (!tmp) {
        tail->next = t;
        t->prev = tail;
        t->next = NULL;
        tail = t;
    }
}