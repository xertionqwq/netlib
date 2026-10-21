#ifndef NETLIB_ACCEPTOR_H
#define NETLIB_ACCEPTOR_H

#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

#include "netlib/inetAddress.h"
#include "netlib/eventloop.h"
#include "netlib/noncopyable.h"
#include "netlib/socket.h"
#include "netlib/log.h"

namespace netlib{

inline EventLoop* checkLoop(EventLoop *loop) {
    if (!loop)
        throw std::invalid_argument("mainLoop must not be null");
    loop->assertInLoopThread();
    return loop;
}

class Acceptor : private Noncopyable{
    /*
    ** Socket owns listen fd, Channel watches incoming connections
    ** noncopyable
    */

public:
    // using alias
    // 调用回调时转交 fd 所有权，由回调负责接管和异常清理；地址引用仅在本次调用有效。
    // 回调执行期间不能销毁 Acceptor，loop 必须比 Acceptor 存活更久。
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

    // 构造、配置、监听、查询和析构均在 loop 所属线程执行。
    Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport = false);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb) {
        loop_->assertInLoopThread();
        newConnectionCallback_ = std::move(cb);
    }

    // 重复调用直接返回；事件注册失败时保持未启动状态，可重试或销毁。
    void listen();
    bool listening() const {
        loop_->assertInLoopThread();
        return listening_;
    }

    static int createNonblockingOrDie();
private:
    void handleRead();

    // variable
    EventLoop *loop_; // 借用指针，不负责释放
    Socket acceptSocket_;
    std::unique_ptr<Channel> acceptChannel_;      // 管理监听事件，fd 由 Socket 持有
    NewConnectionCallback newConnectionCallback_; // 连接建立时负责执行的回调
    bool listening_; // 监听和事件注册均已成功
};

/*
** 根据给出的地址端口创建监听fd
** 同时将监听 fd 和新连接创建回调封装为 channel 并放到 mainLoop 的 poller 进行事件分发
** 事件发生时创建连接 fd, 并将 fd 交给设置的连接回调
*/
inline Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(checkLoop(loop))
    , acceptSocket_(Acceptor::createNonblockingOrDie())
    , acceptChannel_(std::make_unique<Channel>(loop_, acceptSocket_.fd()))
    , listening_(false)
{
    // 设置地址复用选项并绑定，listen() 前不注册读事件。
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reuseport);
    acceptSocket_.bindAddress(listenAddr);

    // 设置读回调为新建连接 fd
    acceptChannel_->setReadCallback([this](Timestamp)
                                    { handleRead(); });
}

inline Acceptor::~Acceptor() {
    loop_->assertInLoopThread();
    acceptChannel_->disableAll();
    acceptChannel_->remove();
}

// 开始监听
inline void Acceptor::listen() {
    loop_->assertInLoopThread();
    if (listening_)
        return;
    acceptSocket_.listen(); // 设置监听
    acceptChannel_->enableReading(); // 向 epoll 注册读事件
    listening_ = true;
}

inline int Acceptor::createNonblockingOrDie() {
    // 创建监听 fd
    const int sockfd = ::socket(AF_INET,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
    if (sockfd < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_FATAL << __FUNCTION__
                         << " listen socket create errno=" << savedErrno;
    }

    return sockfd;
}

// listenfd 有事件发生了, 即有新用户连接
inline void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    InetAddress connAddr(0);
    // 根据监听 fd 创建连接 fd
    const int connfd = acceptSocket_.accept(&connAddr);
    const int savedErrno = errno;

    if (connfd >= 0) {
        // 连接成功, 进入连接回调
        if (newConnectionCallback_) { // 有新连接回调，由回调负责 fd 的后续生命周期
            newConnectionCallback_(connfd, connAddr);
        } else {
            ::close(connfd);
        }
        return;
    }

    // 当前使用 LT 模式，EINTR 时可返回，下一轮继续处理仍就绪的监听事件。
    if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK || savedErrno == EINTR)
        return;

    NETLIB_LOG_ERROR << "Acceptor::handleRead accept fd=" << acceptSocket_.fd()
                     << " errno=" << savedErrno;
    if (savedErrno == EMFILE) {
        NETLIB_LOG_ERROR << "Acceptor::handleRead socket fd limit reached"
                         << " fd=" << acceptSocket_.fd() << " errno=" << savedErrno;
    }
}

}; // netlib

#endif // NETLIB_ACCEPTOR_H
