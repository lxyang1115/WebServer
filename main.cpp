#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <assert.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535 // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大的事件数量
#define TIMESLOT 5 // SIGALARM信号频率

static int pipefd[2];
static sort_timer_list *timer_list;
static int epoll_fd = 0;

void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 添加信号捕捉
void addsig(int sig, void(handler)(int), bool restart = false) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void timer_handler() {
    // 定时处理任务，实际上就是调用tick()函数
    timer_list->tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

extern int setnonblocking(int fd);
extern int addfd(int epoll_fd, int fd, bool one_shot, bool ET, bool rdhup = true);
extern int removefd(int epoll_fd, int fd);
extern void modfd(int epoll_fd, int fd, int ev);

int main(int argc, char *argv[]) {
    if (argc <= 1) {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIPE信号处理
    addsig(SIGPIPE, SIG_IGN);

    // 初始化线程池
    threadpool<http_conn> * pool = NULL;
    try {
        pool = new threadpool<http_conn>();
    } catch(...) {
        exit(-1);
    }

    // 创建数组保存所有客户端信息
    http_conn * users = new http_conn[MAX_FD];

    // 创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // 没有判断
    assert(listenfd >= 0);

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    int ret = 0;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);

    // 监听
    ret = listen(listenfd, 5);
    assert(ret != -1);

    // 创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    epoll_fd = epoll_create(5); // 参数大于0即可，无意义
    assert(epoll_fd != -1);

    // 将监听的文件描述符添加到epoll中
    addfd(epoll_fd, listenfd, false, false);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epoll_fd, pipefd[0], false, true, false);

    // 设置信号处理函数
    addsig(SIGALRM, sig_handler, true);
    addsig(SIGTERM, sig_handler, true);

    bool stop_server = false;
    bool timeout = false;
    alarm(TIMESLOT); // 定时,5秒后产生SIGALARM信号

    http_conn::m_epollfd = epoll_fd;
    timer_list = new sort_timer_list;
    http_conn::m_timer_list = timer_list;

    // printf("listen fd %d, pipe fd %d\n", listenfd, pipefd[0]);
    
    while (!stop_server) {
        int num = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, -1); // 阻塞
        if (num < 0 && errno != EINTR) {  // 中断
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            // printf("fd: %d\n", sockfd);
            if (sockfd == listenfd) {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);
                // printf("connfd: %d\n", connfd);
                if (http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数满
                    // 给客户端写一个信息，服务器内部正忙
                    close(connfd);
                    continue;
                }
                // 将新客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);
            } else if (sockfd == pipefd[0] && events[i].events & EPOLLIN) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
                else {
                    for (int i = 0; i < ret; i++) {
                        switch (signals[i]) {
                            case SIGALRM: {
                                timeout = true;
                                break;
                            } case SIGTERM: {
                                stop_server = true;
                            }
                        }
                    }
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开或者错误等事件
                // 关闭连接
                users[sockfd].close_conn();
            } else if (events[i].events & EPOLLIN) {
                // 读事件发生
                if (users[sockfd].read()) {
                    // 一次性把所有数据读完
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }
                // printf("read\n");
            } else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) {  // 一次性写完
                    users[sockfd].close_conn();
                }
                // printf("write\n");
            }
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if (timeout) {
            timer_handler();
            timeout = false;
        }

    }

    close(epoll_fd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);

    delete[] users;
    delete pool;
    delete timer_list;

    return 0;
}