#include "http_conn.h"

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

int setnonblocking(int fd) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// add fd that need to be listened into epoll
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot) 
    {
        // set oneshot to prevent one socket from being processed by two threads
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);  
}

// remove fd from epoll
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// modify fd to reset the EPOLLONESHOT event so that EPOLLIN event can be triggered
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    // !
    addfd(m_epollfd, m_sockfd, true);
    m_user_count ++;
    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
}

void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // no data
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // the client close the connection
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // // 生成响应
    // bool write_ret = process_write( read_ret );
    // if ( !write_ret ) {
    //     close_conn();
    // }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}



// main state machine
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
        // get a line
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// every line is finished by '\r\n'
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            // the read buffer ends with '\r' -> the line is imcomplete
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;
            // '\r\n' has been read -> the line is complete
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                // end the line
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            // '\n' is not after '\r' -> error
            return LINE_BAD;
        } else if( temp == '\n' )  {
            // the last read buffer ends with '\r' and now the read buffer starts with '\n'
            // -> the line is complete
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                // end the line
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            // '\n' is not after '\r' -> error
            return LINE_BAD;
        }
    }
    // didn't read '\r' or '\n' -> the line is imcomplete
    return LINE_OPEN;
}

// parse the requstline: get the http request mothod, url link and http version
http_conn::HTTP_CODE http_conn::parse_request_line( char* text ) {
    // e.g.
    // text: GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");
    // strpbrk: Find the first occurrence in S of any character in ACCEPT
    // 3 parameters are splited by ' ' or '\t'
    // the idx after the first " \t" is the start of url
    if (! m_url) { 
        return BAD_REQUEST;
    }
    
    *m_url++ = '\0'; // end the text at the method str
    // text: GET\0/index.html HTTP/1.1
    // m_url: /index.html HTTP/1.1

    char* method = text;
    // get the method

    if ( strcasecmp(method, "GET") == 0 ) { // compare the strs ignoring case
        m_method = GET;
    } else {
        return BAD_REQUEST; // only support the get method
    }

    // m_url: /index.html HTTP/1.1
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    // m_url: /index.html\0HTTP/1.1
    // m_version: HTTP/1.1
    
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }

    // if the form of m_url is like http://192.168.80.128:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        m_url = strchr( m_url, '/' );
        // remove "http://" and find the fist '/'
        // m_url: /index.html
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER;
    // finish parsing the requestline and start to parse the header
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers( char* text ) {
     // '\0' means reach a empty line so the header is already parsed
    if( text[0] == '\0' ) {
        // if the http request has the content part
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // if not, a whole request has been gotten
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        // strspn: first character in s1 not existing in s2 and return the idx(length)
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // Content-Length: 112
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // Host: localhost:8080
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// only check if the content is read
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {

}

bool http_conn::write() {
    printf("write\n");
    return true;
}