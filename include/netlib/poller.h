#ifndef NETLIB_POLLER_H
#define NETLIB_POLLER_H


#include <sys/epoll.h>
#include <cassert>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <system_error>
#include <stdexcept>

#include "channel.h"
#include "netlib/noncopyable.h"

namespace netlib
{
class EventLoop;

class Poller : private Noncopyable
{

    /*
    ** Func as epoll(4)
    ** Bind with C++17
    ** Don't own Channel objects
    ** Only own it's epoll_fd
    ** register or cancel fds, never destory/close fds
    ** Encapsulation of events
    ** noncopyable
    */

public:
    // using alias
    using ChannelList = std::vector<Channel *>;

    Poller(EventLoop *loop) : ownerLoop_(loop),
                                epfd_(::epoll_create1(EPOLL_CLOEXEC)) {
        events_.resize(kMaxEvents);
    }
    ~Poller() { close(epfd_); }

    // core function
    // poll the I/O events
    // must be called in one loop thread
    int poll(int timeoutMs, ChannelList &activeChannels);

    // changes the interested I/O events
    // must be called in one loop thread
    // 持有 channel 裸指针, 不拥有所有权
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(const Channel *channel) const {
        auto it = channels_.find(channel->fd());
        return it != channels_.end() && it->second == channel;
    }

    void assertInLoopThread();

private:
    void update(int operation, Channel *channel);

    static constexpr int kNew = -1;    // 未由 Poller 管理
    static constexpr int kAdded = 1;   // 由 Poller 管理，且已注册 epoll
    static constexpr int kDeleted = 2; // 由 Poller 管理，已暂停 epoll 监听

    // 返回就绪 channel 
    void fillActiveChannels(int numEvents,
                            ChannelList &activeChannels) const;

    // using alias, only inner
    using EventList = std::vector<epoll_event>;
    using ChannelMap = std::unordered_map<int, Channel *>;

    // variable
    EventLoop *ownerLoop_;
    EventList events_;  // epoll_wait output buffer, will be changed in every epoll_wait
    ChannelMap channels_; // check channel is inside
    int epfd_;
    static const int kMaxEvents = 128;
};

// function to check the fd that has be ready
inline int Poller::poll(int timeoutMs, ChannelList &activeChannels) {
    // only check, never change events_
    // timeoutMs: -1->Blocking  0->return   num->wait for num ms
    int numEvents = ::epoll_wait(epfd_,
                                    events_.data(),
                                    kMaxEvents,
                                    timeoutMs);
    int savedErrno = errno;  // save before cout/cerr overwrites it

    if (numEvents > 0) {
        std::cout << numEvents << " events has happend" << std::endl;
        fillActiveChannels(numEvents, activeChannels);
    } else if (numEvents == 0) {
        std::cout << "nothing happend" << std::endl;
    } else {
        std::cerr << "epoll_wait error: " << strerror(savedErrno) << std::endl;
    }

    errno = savedErrno;  // 让 EventLoop 能读到原始 errno
    return numEvents; // if error, wait for others to hanle, don't handle fds
}
// function to fill the fd that has be ready
inline void Poller::fillActiveChannels(int numEvents, ChannelList &activeChannels) const {
    for (int i = 0; i < numEvents; i++) {
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels.emplace_back(channel);
    }
}

// function to update pollfds
inline void Poller::updateChannel(Channel *channel) {
    assertInLoopThread();
    assert(channel->ownerLoop() == ownerLoop_);
    const int index = channel->index();

    if (index == kNew) {
        if (channel->isNoneEvent())
            return;

        auto inserted = channels_.emplace(channel->fd(), channel);
        if (!inserted.second)
            throw std::logic_error("Channel fd is already managed by this Poller");
        try {
            update(EPOLL_CTL_ADD, channel);
        } catch (...) {
            channels_.erase(inserted.first);
            throw;
        }
        channel->set_index(kAdded);
        channel->addedToLoop_ = true;
        return;
    }

    assert(hasChannel(channel));
    assert(channel->addedToLoop_);
    if (index == kDeleted) {
        if (!channel->isNoneEvent()) {
            update(EPOLL_CTL_ADD, channel);
            channel->set_index(kAdded);
        }
    } else {
        assert(index == kAdded);
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

// function to remove channels
inline void Poller::removeChannel(Channel *channel) {
    assertInLoopThread();
    assert(channel->ownerLoop() == ownerLoop_);
    assert(channel->isNoneEvent());
    const int index = channel->index();
    auto it = channels_.find(channel->fd());
    if (index == kNew) {
        assert(!channel->addedToLoop_);
        return;
    }
    assert(it != channels_.end() && it->second == channel);
    assert(index == kAdded || index == kDeleted);
    if (index == kAdded)
        update(EPOLL_CTL_DEL, channel);
    channels_.erase(it);
    channel->set_index(kNew);
    channel->addedToLoop_ = false;
}

inline void Poller::update(int operation, Channel *channel) {
    epoll_event event{};
    event.events = channel->events();
    event.data.ptr = channel;
    if (::epoll_ctl(epfd_, operation, channel->fd(), &event) < 0)
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
}
};

#endif // NETLIB_POLLER_H
