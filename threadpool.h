#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<list>
#include<pthread.h>
#include <cstdio>
#include <exception>
#include "locker.h"

template<typename T>
class threadpool {
public:
    threadpool(int threadpool_size = 8, int max_request_num = 10000);
    ~threadpool();
    bool append(T* request);

private:
    static void* worker(void* arg);
    void run();

private:
    // number of threads
    int threadpool_size;

    // vector of threads
    pthread_t * m_threads;

    // number of max request
    int max_request_num;

    //list of request
    std::list<T*> requests;

    // locker of pool
    locker requests_locker;

    // semaphore of pool
    sem requests_sem;

    // flag of the state of the threadpool(run & stop)
    bool m_stop;
};

template<typename T>
threadpool<T>::threadpool(int threadpool_size, int max_request_num) :
threadpool_size(threadpool_size), max_request_num(max_request_num),
m_stop(false), m_threads(NULL) {

    if(threadpool_size <= 0 || max_request_num <= 0) {
        printf("illegal threadpool\n");
        throw std::exception();
    }

    m_threads = new pthread_t[threadpool_size];
    if(!m_threads) {
        printf("can't create threadpool\n");
        throw std::exception();
    }

    //create threads and set them detached
    for(int i = 0; i < threadpool_size; i ++ ) {
        printf("create %d thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {
            delete []m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])) {
            delete []m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool() {
    delete []m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request) {
    requests_locker.lock();
    if(requests.size() > max_request_num) {
        requests_locker.unlock();
        return false;
    }
    requests.push_back(request);
    requests_locker.unlock();
    requests_sem.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg) {
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool; 
}

template<typename T>
void threadpool<T>::run() {
    while(!m_stop) {
        requests_sem.wait();
        requests_locker.lock();
        if(requests.empty()) {
            requests_locker.unlock();
            continue;
        }
        T* request = requests.front();
        requests.pop_front();
        requests_locker.unlock();
        if (request == NULL) {
            continue;
        }
        request->process();
    }
}

#endif