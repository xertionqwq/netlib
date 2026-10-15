#ifndef NETLIB_EPOLLPOLLER_H
#define NETLIB_EPOLLPOLLER_H

#include <sys/epoll.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "netlib/poller.h"
#include "netlib/log.h"

namespace netlib {


class EPollPoller : public Poller {
public:
    explicit EPollPoller(EventLoop *loop);
    ~EPollPoller() override;

    // 重写基类 Poller 的抽象方法
    Timestamp poll(int timeoutMs, ChannelList &activeChannels) override;
    void updateChannel(Channel *channel) override;
    void removeChannel(Channel *channel) override;

private:
    // 填写活跃连接
    void fillActiveChannels(int numEvents, ChannelList &activeChannels) const;
    // 更新 channel, 包括添加/更新/删除 
    void update(int operation, Channel *channel);
    static const char *operationToString(int operation);
    static const char *indexToString(int index);

    // 对应 channel 成员 index_ = -1
    static constexpr int kNew = -1;    // 未由 Poller 管理
    static constexpr int kAdded = 1;   // 由 Poller 管理，且已注册 epoll
    static constexpr int kDeleted = 2; // 由 Poller 管理，已暂停 epoll 监听

    static constexpr int kMaxEvents = 128;
    static constexpr int kInitEventListsSize = 16;

    using EventList = std::vector<epoll_event>;
    EventList events_; // epoll_wait 的就绪事件缓冲区
    int epfd_;
};

inline EPollPoller::EPollPoller(EventLoop *loop) :
    Poller(loop),
    events_(kInitEventListsSize),
    epfd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (epfd_ < 0) {
        NETLIB_LOG_FATAL << "epoll_create error: " << errno << '\n';
        // throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

inline EPollPoller::~EPollPoller() {
    ::close(epfd_);
}

inline void EPollPoller::updateChannel(Channel *channel) {
    assertInLoopThread();
    assert(channel->ownerLoop() == ownerLoop());
    const int index = channel->index();
    NETLIB_LOG_INFO << "func = " << __FUNCTION__
                    << " updateChannel fd=" << channel->fd()
                    << " events=" << channel->events()
                    << " index=" << indexToString(index) << '(' << index << ')';

    if (index == kNew) {
        if (channel->isNoneEvent())
            return;
        // 将 channel 插入 channelMap
        auto inserted = channels_.emplace(channel->fd(), channel);
        if (!inserted.second)
            throw std::logic_error("Channel fd is already managed by this Poller");
        try { // 试图将 channel 添加到 epoll 中
            update(EPOLL_CTL_ADD, channel);
        } catch (...) {
            channels_.erase(inserted.first);
            throw;
        }
        channel->set_index(kAdded); // 设置 index_ 状态
        setChannelAddedToLoop(channel, true);
        return;
    }

    assert(hasChannel(channel)); // 确定 channle 还在 poller 中
    if (index == kDeleted) { // 状态为暂停监听
        if (!channel->isNoneEvent()) {
            update(EPOLL_CTL_ADD, channel); // 注册
            channel->set_index(kAdded);
        }
    } else { // index == kAdded 还在监听中
        assert(index == kAdded);
        if (channel->isNoneEvent()) { // 不关心事件
            // 暂停内核监听，仍保留 channels_ 中的管理记录
            update(EPOLL_CTL_DEL, channel); // 删除
            channel->set_index(kDeleted);
        } else { // 还有事件
            update(EPOLL_CTL_MOD, channel); // 更新
        }
    }
}

inline void EPollPoller::removeChannel(Channel *channel) {
    assertInLoopThread();
    assert(channel->ownerLoop() == ownerLoop());
    assert(channel->isNoneEvent());
    const int index = channel->index();
    NETLIB_LOG_INFO << "func = " << __FUNCTION__
                    << " removeChannel fd=" << channel->fd()
                    << " index=" << indexToString(index) << '(' << index << ')';

    // 未注册或已移除时无需操作，避免误删同一 fd 的其他 Channel
    if (index == kNew)
        return;

    auto it = channels_.find(channel->fd());
    assert(it != channels_.end() && it->second == channel);
    assert(index == kAdded || index == kDeleted);
    if (index == kAdded) // 状态为监听
        update(EPOLL_CTL_DEL, channel); // 删除
    channels_.erase(it);      // 从 channelMap 中删除
    channel->set_index(kNew); // 设置为未添加
    setChannelAddedToLoop(channel, false);
}

// 更新 channel 通道 epoll_ctl add/mod/del
inline void EPollPoller::update(int operation, Channel *channel) {
    // 描述监听事件，并保存就绪时返回的 Channel 指针
    epoll_event event{};
    event.events = channel->events();
    event.data.ptr = channel;
    const int fd = channel->fd();
    const int index = channel->index();
    const char *opName = operationToString(operation);
    NETLIB_LOG_DEBUG << "epoll_ctl op=" << opName << '(' << operation << ')'
                     << " epfd=" << epfd_ << " fd=" << fd
                     << " events=" << event.events
                     << " index=" << indexToString(index) << '(' << index << ')';

    // DEL 不使用事件描述，失败时与 ADD/MOD 一样报告异常
    if (::epoll_ctl(epfd_, operation, fd,
                    operation == EPOLL_CTL_DEL ? nullptr : &event) < 0) {
        const int savedErrno = errno;
        std::system_error error(
            savedErrno, std::generic_category(),
            std::string("epoll_ctl op=") + opName + '(' + std::to_string(operation) +
                ") epfd=" + std::to_string(epfd_) + " fd=" + std::to_string(fd) +
                " events=" + std::to_string(event.events) + " index=" +
                indexToString(index) + '(' + std::to_string(index) + ')');
        NETLIB_LOG_ERROR << error.what() << " errno=" << savedErrno;
        throw error;
    }
}

inline const char *EPollPoller::operationToString(int operation) {
    switch (operation) {
        case EPOLL_CTL_ADD: return "ADD";
        case EPOLL_CTL_MOD: return "MOD";
        case EPOLL_CTL_DEL: return "DEL";
        default: return "UNKNOWN";
    }
}

inline const char *EPollPoller::indexToString(int index) {
    switch (index) {
        case kNew: return "NEW";
        case kAdded: return "ADDED";
        case kDeleted: return "DELETED";
        default: return "UNKNOWN";
    }
}
};

#endif // NETLIB_EPOLLPOLLER_H
