#include "http_conn.h"

#define TIMESLOT 5 // SIGALARM信号频率

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 路径
const char * root = "/home/lxy1115/Desktop/Linux-lesson/webserver";
const char * doc_root = "/home/lxy1115/Desktop/Linux-lesson/webserver/resources";

int http_conn::m_epollfd = -1; // 所有socket上的事件都被注册到一个epoll中
int http_conn::m_user_count = 0; // 统计用户数量
sort_timer_list *http_conn::m_timer_list = NULL;

// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}

// 向epoll文件添加需要监听的文件描述符
int addfd(int epoll_fd, int fd, bool one_shot, bool ET, bool rdhup = true) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN;
    if (rdhup) event.events |= EPOLLRDHUP; // RDHUP:异常断开
    if (one_shot) event.events |= EPOLLONESHOT;
    if (ET) event.events |= EPOLLET;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
    return 1;
}

// 从epoll中删除文件描述符
int removefd(int epoll_fd, int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
    return 1;
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epoll_fd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化
void http_conn::init(int sockfd, const sockaddr_in & addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true, true);
    m_user_count++; // 总用户数增加

    // 初始化计时器
    // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
    if (!m_timer) {
        m_timer = new util_timer;
        m_timer->user_conn = this;  
        m_timer_list->add_timer(m_timer);
    }
    m_timer->expire = (int)time(NULL) + 3 * TIMESLOT;
    // printf("m_timer: %d\n", m_timer);

    init();
}

void http_conn::init() {
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;
    m_checked_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_linger = false;
    m_content_length = 0;
    m_content_start = 0;
    m_file_address = NULL;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_file, FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }

    if (m_timer) {
        m_timer_list->del_timer(m_timer);
        // printf("delete\n");
        m_timer = NULL;
    }
}

// 调整计时器
void http_conn::adjust_timer() {
    if (m_timer) {
        m_timer->expire = (int)time(NULL) + 3 * TIMESLOT;
        // printf("adjust timer once\n");
        m_timer_list->adjust_timer(m_timer);
    }
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    // 读取到的字节
    int bytes = 0;
    while (true) {
        bytes = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据
                break;
            } else {
                return false;
            }
        } else if (bytes == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes;
    }
    // printf("读取到数据：%s\n", m_read_buf);
    // printf("一次性读完数据\n");
    // 更新定时器
    adjust_timer();
    return true;
}

// 写HTTP响应
bool http_conn::write() {
    int tmp = 0;
    
    // 没有数据发送，不会触发
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (true) {
        tmp = writev(m_sockfd, m_iv, m_iv_count);
        if (tmp <= -1) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                // 重新再发
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            } else {
                // 出错，释放空间，关闭连接
                unmap();
                return false;
            }
        }

        bytes_have_send += tmp;
        bytes_to_send -= tmp;

        if (bytes_to_send <= 0) {
            // 发送结束
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if (m_linger) {
                init();
                // 更新定时器
                adjust_timer();
                return true;
            } else return false;
        } else if (bytes_have_send >= m_iv[0].iov_len) {
            // 发送完成m_iv[1]
            m_iv[0].iov_len = 0;
            m_iv[1].iov_len = bytes_to_send;
            m_iv[1].iov_base = m_file_address + m_file_stat.st_size - bytes_to_send;
        } else {
            m_iv[0].iov_len -= tmp;
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
        }
    }

    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char * text = 0;
    while ( (m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) 
        || ((line_status = parse_line()) == LINE_OK) ) {
        // 解析到一行完整的数据，或者解析到请求体，也是完整的数据
        // 获取一行数据
        text = getline();
        m_start_line = m_checked_index;
        // printf("get one http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            } case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            } case CHECK_STATE_CONTENT: {
                ret = parse_contents(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            } default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 解析HTTP请求行，获取请求方法、目标url、HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char * text) {
    // GET / HTTP/1.1
    m_url = strpbrk(text, " \t");
    if (!m_url) return BAD_REQUEST;
    *m_url = '\0';
    m_url++;
    // GET\0/ HTTP/1.1
    char * method = text;
    if (strcasecmp(text, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(text, "POST") == 0) {
        m_method = POST;
    } else return BAD_REQUEST;

    // / HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version = '\0';
    m_version++;
    // /\0HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    // http://192.168.72.130/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') return BAD_REQUEST;

    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

// 解析HTTP请求头
http_conn::HTTP_CODE http_conn::parse_headers(char * text) {
    // 遇到空行
    if (text[0] == '\0') {
        if (m_content_length != 0) { // 有请求体
            m_check_state = CHECK_STATE_CONTENT;
            m_content_start = m_checked_index;
            return NO_REQUEST;
        } else return GET_REQUEST; // 没有请求体，结束
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text = strpbrk(text, " \t") + 1;
        m_host = text;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text = strpbrk(text, " \t") + 1;
        if (strncasecmp(text, "keep-alive", 10) == 0) m_linger = true;
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text = strpbrk(text, " \t") + 1;
        m_content_length = atoi(text);
    } else {
        // printf( "skip! unknown header %s\n", text);
    }
    return NO_REQUEST;
}

// 没有解析
http_conn::HTTP_CODE http_conn::parse_contents(char * text) {
    if (m_read_idx >= m_checked_index + m_content_length) {
        text[m_content_length] = '\0';
        // printf("text: %s\n", text);
        // printf("read idx: %d, check idx: %d, content l: %d, content start: %d\n", m_read_idx, m_checked_index, m_content_length, m_content_start);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char tmp;
    for (; m_checked_index < m_read_idx; ++m_checked_index) {
        tmp = m_read_buf[m_checked_index];
        if (tmp == '\r') {
            if ((m_checked_index + 1) == m_read_idx) return LINE_OPEN;
            else if (m_read_buf[m_checked_index + 1] == '\n') {
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (tmp == '\n') {
            if (m_checked_index > 1 && m_read_buf[m_checked_index-1] == '\r') {
                m_read_buf[m_checked_index-1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_file文件的相关的状态信息，-1失败，0成功
    if (stat(m_file, &m_file_stat) < 0) {
        printf("wrong path!\n");
        return NO_RESOURCE;
    }

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        printf("forbidden access!\n");
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        printf("it's a directory!\n");
        return BAD_REQUEST;
    }

    // 读文件
    int fd = open(m_file, O_RDONLY);
    // 内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;
    va_list arglist;
    va_start(arglist, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arglist);
    if (len > (WRITE_BUFFER_SIZE - m_write_idx - 1)) return false;
    m_write_idx += len;
    va_end(arglist);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
}

bool http_conn::add_headers(int content_length) {
    bool f = add_content_length(content_length);
    f = f && add_content_type();
    f = f && add_linger();
    f = f && add_blank_line();
    return f;
}

bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type: text/html\r\n");
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger ? "keep-alive" : "close"));
}

bool http_conn::add_blank_line() {
    return add_response("\r\n");
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(http_conn::HTTP_CODE ret) {
    bool ok = false;
    switch (ret) {
        case INTERNAL_ERROR: {
            bool f = add_status_line(500, error_500_title);
            f = f && add_headers(strlen(error_500_form));
            f = f && add_content(error_500_form);
            if (!f) return false;
            break;
        } case BAD_REQUEST: {
            bool f = add_status_line(400, error_400_title);
            f = f && add_headers(strlen(error_400_form));
            f = f && add_content(error_400_form);
            if (!f) return false;
            break;
        } case NO_RESOURCE: {
            bool f = add_status_line(404, error_404_title);
            f = f && add_headers(strlen(error_404_form));
            f = f && add_content(error_404_form);
            if (!f) return false;
            break;
        } case FORBIDDEN_REQUEST: {
            bool f = add_status_line(403, error_403_title);
            f = f && add_headers(strlen(error_403_form));
            f = f && add_content(error_403_form);
            if (!f) return false;
            break;
        } case FILE_REQUEST: {
            bool f = add_status_line(200, ok_200_title);
            f = f && add_headers(m_file_stat.st_size);
            if (!f) return false;
            ok = true;
            break;
        } default: {
            return false;
        }
    }

    // printf("发送响应：%s\n", m_write_buf);
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;

    if (ok) {
        // printf("响应体：%s\n", m_file_address);
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count += 1;
        bytes_to_send += m_file_stat.st_size;
    }

    return true;
}

// 由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        // 修改socket epoll
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    
    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
        // printf("close\n");
        return;
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT); // 可以写了
}

void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

