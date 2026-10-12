#ifndef NETLIB_CHANNEL_H
#define NETLIB_CHANNEL_H


#include <sys/epoll.h>
#include <cassert>
#include <functional>
#include <memory>

#include "netlib/noncopyable.h"
#include "netlib/timestamp.h"
#include "log.h"

namespace netlib{
class EventLoop; // 前置声明
class Channel : private Noncopyable{

    /*
    ** A selectable I/O channel
    ** Channel 理解为通道, 封装 sockfd 和感兴趣的 event
    ** 还绑定了 poller 返回的具体时间
    ** noncopyable
    ** Bitwise: |= set, &= ~ unset, == check, & test
    */
public:
    // using alias
    using EventCallBack = std::function<void()>;
    using ReadEventCallBack = std::function<void(Timestamp)>;

    Channel(EventLoop *loop, int fd); // the only constructor
    ~Channel() {
        assert(!eventHandling_);
        assert(!addedToLoop_);
        // TDDO assert()
        // 查看 channel 是否在 loop 所属线程内被析构
    }

    // 核心函数, fd 得到 poller 通知, 调用相关回调处理事件
    void handleEvent(Timestamp);
    // set callback events type
    void setReadCallback(ReadEventCallBack cb) {
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

    // 防止 channel 被手动 remove 时 channel 还在执行回调操作
    void tie(const std::shared_ptr<void> &);

    // function to variable
    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int rev) { revents_ = rev; }
    EventLoop *ownerLoop() { return loop_; }

    // 设置 fd 相应事件状态
    void enableReading() { 
        events_ |= kReadEvent;
        update(); // 提醒 poller 进行更新
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

    // 当前 fd 事件状态
    bool isNoneEvent() const { return events_ == kNoneEvent; } // 没有关注的事件
    bool isReading() const { return events_ & kReadEvent; }
    bool isWriting() const { return events_ & kWritEvent; }

    // epoll 注册状态：kNew / kAdded / kDeleted
    int index() { return index_; }
    void set_index(int idx) { index_ = idx; }
    void remove(); // 从 poller 中移除

private:
    friend class Poller;

    void update(); // 更新自己在 poller 里的状态
    void handleEventWithGuard(Timestamp receiveTime);

    // 对感兴趣事件的状态描述
    static constexpr int kNoneEvent = 0;
    static constexpr int kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
    static constexpr int kWritEvent = EPOLLOUT;

    // variable about fd and events
    EventLoop *loop_;    // 事件循环
    const int fd_;       // Poller 监听对象
    int events_;         // 注册 fd 感兴趣的事件
    int revents_;        // Poller 返回的具体发生的事件
    int index_;          // epoll 注册状态

    // 负责检测 channel 是否存活, 避免相关资源重复释放
    std::weak_ptr<void> tie_;
    bool tied_;
    bool eventHandling_; // 检测是否正在处理回调事件
    bool addedToLoop_;   // 是否由 loop 管理，禁用事件后仍须 remove

    // EventCallBack functions
    // Channel 获知 fd 以及发生的 revent 事件, 所以负责调用回调
    ReadEventCallBack readCallback_;
    EventCallBack writeCallback_;
    EventCallBack closeCallback_;
    EventCallBack errorCallback_;
};

// constructor
inline Channel::Channel(EventLoop *loop, int fdArg) :
    loop_(loop), 
    fd_(fdArg), 
    events_(0), 
    revents_(0), 
    index_(-1),
    tied_(false),
    eventHandling_(false),
    addedToLoop_(false) {}
// update() and remove() defined in eventloop.h

// 绑定 obj 共享对象, 作为观察者查看对象是否存活
// channel 的 tie 方法何时会被调用过
inline void Channel::tie(const std::shared_ptr<void> &obj) {
    tie_ = obj;
    tied_ = true;
}

// 根据 poller 通知的 channel 发生的具体事件, 由 channel 调用具体的回调事件
inline void Channel::handleEventWithGuard(Timestamp receiveTime) {
    NETLIB_LOG_INFO << "channel handEvent revents: " << revents_;

    eventHandling_ = true;
    // HUP 伴随可读事件时，先消费剩余数据；RDHUP 交给读回调处理 EOF
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_)
            closeCallback_();
        eventHandling_ = false;
        return;
    } // time to close
    if (revents_ & EPOLLERR) {
        if (errorCallback_)
            errorCallback_();
        if (isNoneEvent()) {
            eventHandling_ = false;
            return;
        }
    } // something error
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_)
            readCallback_(receiveTime);
        if (isNoneEvent()) {
            eventHandling_ = false;
            return;
        }
    } // time to read
    if ((revents_ & EPOLLOUT) && isWriting()) {
        if (writeCallback_)
            writeCallback_();
    } // time to write
    eventHandling_ = false;
}
// core function
inline void Channel::handleEvent(Timestamp receiveTime) {
    if (tied_) { // 该 channel 被绑定过
        std::shared_ptr<void> guard;
        guard = tie_.lock(); // 查看是否存活
        if (guard)
            handleEventWithGuard(receiveTime);
    } else {
        handleEventWithGuard(receiveTime);
    }
}
};

#endif //NETLIB_CHANNEL_H
