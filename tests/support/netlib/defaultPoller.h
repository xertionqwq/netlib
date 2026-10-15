#ifndef NETLIB_TEST_DEFAULTPOLLER_H
#define NETLIB_TEST_DEFAULTPOLLER_H

// 仅供 EventLoop 测试：用 poll(2) 提供等待，独立验证尚未接通 epoll 的循环层。
// CMake 仅对 test_eventloop 设置此头文件的优先搜索路径。
#include <atomic>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "netlib/poller.h"

namespace netlib {
namespace test {

inline std::atomic_bool failNextRegistration{false};
inline std::atomic_bool failNextPoll{false};
inline std::atomic_int pollCalls{0};

class TestPoller : public Poller {
public:
    explicit TestPoller(EventLoop* loop) : Poller(loop) {}

    Timestamp poll(int timeoutMs, ChannelList& activeChannels) override {
        assertInLoopThread();
        ++pollCalls;
        if (failNextPoll.exchange(false))
            throw std::runtime_error("injected poll failure");

        std::vector<pollfd> descriptors;
        ChannelList channels;
        for (const auto& entry : channels_) {
            Channel* channel = entry.second;
            if (channel->isNoneEvent())
                continue;
            short events = 0;
            if (channel->isReading()) events |= POLLIN;
            if (channel->isWriting()) events |= POLLOUT;
            descriptors.push_back({channel->fd(), events, 0});
            channels.push_back(channel);
        }
        const int result = ::poll(descriptors.data(), descriptors.size(), timeoutMs);
        if (result < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "test poll");
        for (size_t i = 0; result > 0 && i < descriptors.size(); ++i) {
            const short events = descriptors[i].revents;
            if (events == 0)
                continue;
            int revents = 0;
            if (events & POLLIN) revents |= EPOLLIN;
            if (events & POLLOUT) revents |= EPOLLOUT;
            if (events & POLLHUP) revents |= EPOLLHUP;
            if (events & (POLLERR | POLLNVAL)) revents |= EPOLLERR;
            channels[i]->set_revents(revents);
            activeChannels.push_back(channels[i]);
        }
        return Timestamp::now();
    }

    void updateChannel(Channel* channel) override {
        assertInLoopThread();
        NETLIB_ASSERT(channel->ownerLoop() == ownerLoop());
        auto it = channels_.find(channel->fd());
        if (it == channels_.end()) {
            if (channel->isNoneEvent())
                return;
            if (failNextRegistration.exchange(false))
                throw std::system_error(EIO, std::generic_category(),
                                        "injected registration failure");
            channels_.emplace(channel->fd(), channel);
            setChannelAddedToLoop(channel, true);
        } else {
            NETLIB_ASSERT(it->second == channel);
        }
        channel->set_index(channel->isNoneEvent() ? 2 : 1);
    }

    void removeChannel(Channel* channel) override {
        assertInLoopThread();
        NETLIB_ASSERT(channel->isNoneEvent());
        if (channel->index() == -1)
            return;
        auto it = channels_.find(channel->fd());
        NETLIB_ASSERT(it != channels_.end() && it->second == channel);
        channels_.erase(it);
        channel->set_index(-1);
        setChannelAddedToLoop(channel, false);
    }
};

} // namespace test

inline Poller* Poller::newDefaultPoller(EventLoop* loop) {
    return new test::TestPoller(loop);
}

} // namespace netlib

#endif // NETLIB_TEST_DEFAULTPOLLER_H
