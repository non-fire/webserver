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
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // max num of fd
#define MAX_EVENT_NUMBER 10000  // max num of listened events

extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("port number need to be provided");
        return 1;
    }

    int port = atoi(argv[1]);
    addsig( SIGPIPE, SIG_IGN );

    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];

    int ret = 0;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert(listen >= 0);

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd; 

    while(true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0 && errno != EINTR) {
            printf( "epoll failure\n" );
            break;
        }

        for(int i = 0; i < number; i ++) {
            int socketfd = events[i].data.fd;
            if (socketfd == listenfd) {

                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                int connfd = accept(socketfd, (struct sockaddr*)& client_address, &client_addrlength);
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }
                users[connfd].init( connfd, client_address);

            } else if(events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR) ) {

                users[socketfd].close_conn();

            } else if(events[i].events & EPOLLIN) {

                if(users[socketfd].read()) {
                    pool->append(users + socketfd);
                } else {
                    users[socketfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {

                if( !users[socketfd].write() ) {
                    users[socketfd].close_conn();
                }

            }

        }
    }
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}