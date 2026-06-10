#ifndef NETLIB_POLLER_H
#define NETLIB_POLLER_H


#include <sys/epoll.h>
#include <cassert>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <unordered_map>

#include "channel.h"

namespace netlib
{
class EventLoop;

class Poller
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

    // delete construct/assign copy
    Poller(const Poller &others) = delete;
    Poller &operator=(const Poller &others) = delete;

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
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);

    void assertInLoopThread();

private:
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
    // assertInLoopThread();

    struct epoll_event ev;
    ev.events = channel->events();
    ev.data.ptr = channel;
    auto check = channels_.find(channel->fd());

    if (channel->isNoneEvent()) {
        // cur ch don't care about anything
        if (check != channels_.end())
            removeChannel(channel);
        return;
    }

    if (check == channels_.end()) { // new one, add
        int ret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD,
                            channel->fd(), &ev);
        if (ret == -1) {
            std::cerr << "add channel error" << std::endl;
        } else {
            channels_[channel->fd()] = channel; // link
        }
    } else { // old one, mod
        assert(channels_[channel->fd()]== channel); // to ensure is self
        int ret = ::epoll_ctl(epfd_, EPOLL_CTL_MOD,
                            channel->fd(), &ev);
        if (ret == -1) {
            std::cerr << "mod channel error" << std::endl;
            assert(ret == 0);
            removeChannel(channel);
        } 
    }
}

// function to remove channels
inline void Poller::removeChannel(Channel *channel) {
    assertInLoopThread();
    // avoid remove twice
    assert(channels_.find(channel->fd()) != channels_.end());
    assert(channels_[channel->fd()] == channel);
    assert(channel->isNoneEvent());
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL,
                channel->fd(), nullptr);
    channels_.erase(channel->fd());
}
};

#endif // NETLIB_POLLER_H