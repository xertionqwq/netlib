#ifndef NETLIB_EVENTLOOP_H
#define NETLIB_EVENTLOOP_H

#include <thread>
#include <cassert>
#include <memory>
#include <cerrno>

#include "netlib/poller.h"
#include "netlib/channel.h"

namespace netlib{

class EventLoop{

    /*
    ** ask for activeChannels, use it's eventCallback
    ** Bind with C++17
    ** noncopyable
    */
    EventLoop(const EventLoop &others) = delete;
    EventLoop &operator=(const EventLoop &others) = delete;

public:
    void assertInLoopThread() const {
        assert(threadId_ == std::this_thread::get_id());
    }
    bool isInLoopTread() const {
        return threadId_ == std::this_thread::get_id();
    }

    EventLoop();
    EventLoop(int);
    ~EventLoop();

    // getter/setter
    int pollTimeoutMs() { return pollTimeoutMs_; }
    void setPollTimeoutMs(int ms) {
        assert(ms >= 0 || ms == -1);
        pollTimeoutMs_ = ms;
    }

    void loop(); // core function
    void quit() { quit_ = true; }

    // forward to Poller
    void updateChannel(Channel* ch) { poller_->updateChannel(ch); }
    void removeChannel(Channel* ch) { poller_->removeChannel(ch); }

private:
    std::thread::id threadId_;
    std::unique_ptr<Poller> poller_;
    std::vector<Channel *> activeChannels_;
    int pollTimeoutMs_ = -1;
    bool loop_ = false;
    bool quit_ = false;
};

EventLoop::EventLoop() : threadId_(std::this_thread::get_id()),
                         poller_(std::make_unique<Poller>(this)) {}
EventLoop::EventLoop(int timeWaitMs) : threadId_(std::this_thread::get_id()),
                         poller_(std::make_unique<Poller>(this)) {
    assert(timeWaitMs >= 0 || timeWaitMs == -1);
    pollTimeoutMs_ = timeWaitMs;
}
EventLoop::~EventLoop() {
    assert(loop_ == false); // is time to quit
    quit_ = true;
}

inline void EventLoop::loop() {
    assertInLoopThread();
    assert(quit_ == false);

    loop_ = true;
    while (!quit_) {
        activeChannels_.clear();
        int numEvents = poller_->poll(pollTimeoutMs_, activeChannels_);
        if (numEvents > 0) {
            for (size_t i = 0; i < activeChannels_.size(); i++) {
                activeChannels_[i]->handleEvent();
            }
        } else if (numEvents == 0) {
            continue;
        } else {
            if (errno == EINTR)
                continue;
            else
                quit_ = true;
        }
    }
    loop_ = false;
}

// Channel 的方法需要 EventLoop 完整定义
inline void Channel::update() {
    loop_->updateChannel(this);
}
inline void Channel::remove() {
    disableAll();  // update() 内调 Poller::updateChannel → isNoneEvent → removeChannel
}

inline void Poller::assertInLoopThread() {
    ownerLoop_->assertInLoopThread();
}
};

#endif // NETLIB_EVENTLOOP_H
