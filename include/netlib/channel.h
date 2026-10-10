#ifndef NETLIB_CHANNEL_H
#define NETLIB_CHANNEL_H


#include <sys/epoll.h>
#include <cassert>
#include <functional>
#include <memory>

#include "netlib/noncopyable.h"

namespace netlib{
class EventLoop; // predeclaration

class Channel : private Noncopyable{

    /*
    ** A selectable I/O channel
    ** Bind with C++17
    ** Encapsulation of FD and EventCallBack
    ** noncopyable
    ** Bitwise: |= set, &= ~ unset, == check, & test
    */
public:
    // using alias
    using EventCallBack = std::function<void()>;

    Channel(EventLoop *loop, int fd); // the only constructor
    ~Channel() { assert(!eventHandling_); }

    void handleEvent(); // the core function, to assign tasks
    // set callback events type
    void setReadCallback(EventCallBack cb) {
        readCallback_ = std::move(cb);
    }
    void setWriteCallback(EventCallBack cb) {
        writeCallback_ = std::move(cb);
    }
    void setErrorCallback(EventCallBack cb) {
        errorCallback_ = std::move(cb);
    }
    void setCloseCallback(EventCallBack cb) {
        closeCallback_ = std::move(cb);
    }

    // function to variable
    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int rev) { revents_ = rev; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    EventLoop *ownerLoop() { return loop_; }

    // function to set epoll mod
    void enableReading() { 
        events_ |= kReadEvent;
        update();
    }
    void enableWriting() {
        events_ |= kWritEvent;
        update();
    }
    void disableWriting() {
        events_ &= ~kWritEvent;
        update();
    }
    void disableAll() {
        events_ = kNoneEvent;
        update();
    }
    bool isWriting() const {
        return events_ & kWritEvent;
    }

    // for Poller(pollpoller, not epollpoller)
    // 为 poll 准备的轮询检查函数
    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }
    void remove();

private:
    void update(); // 更新自己在 epoll 里的状态
    // set the epoll mod
    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWritEvent;

    // variable about fd and events
    EventLoop *loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_; // will be use in Poller, to check pos in Pollfds_
    bool eventHandling_; // detect whether handling events

    // EventCallBack functions
    EventCallBack readCallback_;
    EventCallBack writeCallback_;
    EventCallBack closeCallback_;
    EventCallBack errorCallback_;
};

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWritEvent = EPOLLOUT;

// constructor
Channel::Channel(EventLoop *loop, int fdArg) : 
    loop_(loop), 
    fd_(fdArg), 
    events_(0), 
    revents_(0), 
    index_(-1) {}
// update() and remove() defined in eventloop.h

// core function
inline void Channel::handleEvent() {
    eventHandling_ = true;
    // focus on: the order of events shouldn't be changed
    // this means channel can handle kinds of events by order
    if (revents_ & (EPOLLIN | EPOLLPRI)) {
        if (readCallback_)
            readCallback_();
    } // time to read
    if (revents_ & EPOLLOUT) {
        if (writeCallback_)
            writeCallback_();
    } // time to write
    if (revents_ & (EPOLLHUP | EPOLLRDHUP)) {
        if (closeCallback_)
            closeCallback_();
    } // time to close
    if (revents_ & EPOLLERR) {
        if (errorCallback_)
            errorCallback_();
    } // something error
    eventHandling_ = false;
}
};

#endif //NETLIB_CHANNEL_H
