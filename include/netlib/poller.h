#ifndef NETLIB_POLLER_H
#define NETLIB_POLLER_H


#include <vector>
#include <unordered_map>

#include "channel.h"
#include "netlib/noncopyable.h"

namespace netlib
{
class EventLoop;

class Poller : private Noncopyable
{

    /*
    ** 多路事件分发器的核心 IO 复用模块
    ** Don't own Channel objects
    ** register or cancel fds, never destory/close fds
    ** noncopyable
    */

public:
    // using alias
    using ChannelList = std::vector<Channel *>;

    explicit Poller(EventLoop *loop) : ownerLoop_(loop) {}
    virtual ~Poller() = default;

    // core function
    // poll the I/O events
    // must be called in one loop thread
    virtual Timestamp poll(int timeoutMs, ChannelList &activeChannels) = 0;

    // changes the interested I/O events
    // must be called in one loop thread
    // 持有 channel 裸指针, 不拥有所有权
    virtual void updateChannel(Channel *channel) = 0;
    virtual void removeChannel(Channel *channel) = 0;
    
    // 判断参数 channel 是否在当前 Poller 中
    bool hasChannel(const Channel *channel) const {
        auto it = channels_.find(channel->fd());
        return it != channels_.end() && it->second == channel;
    }

    // EventLoop 可通过该接口获取默认 IO 复用的具体实现
    static Poller *newDefaultPoller(EventLoop *loop);

    void assertInLoopThread();

protected:
    EventLoop *ownerLoop() const { return ownerLoop_; }
    void setChannelAddedToLoop(Channel *channel, bool added) {
        channel->addedToLoop_ = added;
    }

    // using alias, only inner
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_; // check channel is inside

private:
    // variable
    EventLoop *ownerLoop_;
};
};

#endif // NETLIB_POLLER_H
