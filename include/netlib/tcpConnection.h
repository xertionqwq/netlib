#ifndef NETLIB_TCPCONNECTION_H
#define NETLIB_TCPCONNECTION_H

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <string>
#include <iostream>
#include <utility>
#include <functional>

#include "netlib/Callbacks.h"
#include "netlib/eventloop.h"
#include "netlib/inetAddress.h"
#include "netlib/buffer.h"
#include "netlib/socket.h"
#include "netlib/timestamp.h"
#include "netlib/noncopyable.h"
#include "netlib/log.h"

namespace netlib{

/*
** TcpServer -> Acceptor -> 有一个新用户连接, 通过 accept 函数拿到 connfd
** -> TcpConnection 设置回调 -> channel 注册到 -> Poller 监听, 事件发生返回活跃channel -> EventLoop 里面让 channel->handleEvent()
*/
class TcpConnection : private Noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {

public:
    TcpConnection(EventLoop *loop,
                  const std::string &name,
                  int sockfd,
                  const InetAddress &localAddr,
                  const InetAddress &peerAddr);
    ~TcpConnection();

    EventLoop *getLoop() const { return loop_; }
    const std::string &name() const { return name_; }
    const InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress() const { return peerAddr_; }

    // 查看连接状态
    bool connected() const { return state_ == StateE::kConnected; }
    bool disconnected() const { return state_ == StateE::kDisconnected; }

    // 空消息不发送；跨线程时复制消息并持有连接，实际执行时再次检查连接状态。
    void send(const void *message, size_t len);
    void send(const std::string &message);
    // 已连接时接管可读数据并在调用线程清空 Buffer；调用方仍拥有 Buffer 对象。
    void send(Buffer *buf);

    // the setter
    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }
    void setCloseCallback(CloseCallback cb) {
        closeCallback_ = std::move(cb); 
    }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb) {
        highWaterMarkCallback_ = std::move(cb);
    }

    // 连接建立
    void connectEstablished();
    // 连接销毁
    void connectDestroyed();

    // 等待输出缓冲排空后关闭写端，仍可接收对端数据。
    void shutdown();
    // 排队终止连接，不等待输出缓冲；fd 在 Socket 析构时释放。
    void forceClose();

private:
    void shutdownInLoop();
    void forceCloseInLoop();
    void sendInLoop(const std::string &message);
    void sendInLoop(const void *message, size_t len);

    enum class StateE {
        kConnecting,   // 正在连接
        kConnected,    // 已连接
        kDisconnected, // 已断开
        kDisconnecting // 正在断开连接
    };

    std::string enumToStr() const {
        switch (state_) {
        case StateE::kConnecting:
            return "kConnecting";
            break;
        case StateE::kConnected:
            return "kConnected";
            break;
        case StateE::kDisconnected:
            return "kDisconnected";
            break;
        case StateE::kDisconnecting:
            return "kDisconnecting";
            break;
        default:
            return "Unknown Type";
            break;
        }
    }

    void setState(StateE s) { state_ = s; }
    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();

private:
    // variable
    EventLoop *loop_; // 连接所属的 I/O loop, 零工作线程时可以为 baseLoop
    const std::string name_;
    std::atomic<StateE> state_; // 当前连接的状态, 原子变量
    bool reading_;
    bool writeShutdown_; // 仅在所属 loop 线程访问，记录是否已请求关闭写端

    // 这里与 Acceptor 类似     Acceptor -> mainLoop
    // TcpConnection -> subLoop
    std::unique_ptr<Socket> socket_;   // 掌管连接的 fd 生命
    std::unique_ptr<Channel> channel_; // 掌管连接 fd 的回调

    const InetAddress localAddr_;      // 连接使用的地址
    const InetAddress peerAddr_;

    // the Buffer, 读写缓冲区
    Buffer inputBuf_;  // 收缓冲区
    Buffer outputBuf_; // 发缓冲区

    // Callback, 设置的回调函数
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    size_t highWaterMark_; // 水位线
};

inline TcpConnection::TcpConnection(EventLoop *loop,
                                    const std::string &nameArg,
                                    int sockfd,
                                    const InetAddress &localAddr,
                                    const InetAddress &peerAddr)
    : loop_(checkLoop(loop)) // 仅检查非空，允许在其他线程构造后交给 I/O loop 建立
    , name_(nameArg)
    , state_(StateE::kConnecting)
    , reading_(true)
    , writeShutdown_(false)
    , socket_(std::make_unique<Socket>(sockfd))
    , channel_(std::make_unique<Channel>(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024) // 64M
{
    // 给 channel 设置相应的回调函数
    // poller 通知 channel 感兴趣的事件发生了
    // EventLoop 内 channel 执行 handleEvent()
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1)
    );
    channel_->setWriteCallback([this]
                               { handleWrite(); });
    channel_->setCloseCallback([this]
                               { handleClose(); });
    channel_->setErrorCallback([this]
                               { handleError(); });

    NETLIB_LOG_DEBUG << "TcpConnection::ctor["
                     << name_ << "] at " << this
                     << " fd=" << sockfd << '\n';
    socket_->setKeepAlive(true);
}

inline TcpConnection::~TcpConnection() {
    NETLIB_LOG_DEBUG << "TcpConnection::dtor["
                        << name_ << "] at " << this
                        << " fd=" << channel_->fd()
                        << " state=" << enumToStr() << '\n';
    NETLIB_ASSERT(state_ == StateE::kDisconnected);
}

inline void TcpConnection::handleRead(Timestamp receiveTime) {
    loop_->assertInLoopThread();
    int savedErrno = 0;
    auto n = inputBuf_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) { // 有数据可读
        if (messageCallback_) // 已建立连接的用户有可读事件发生了, 调用用户传入的回调操作 onMessage
            messageCallback_(shared_from_this(), &inputBuf_, receiveTime);
    }
    else if (n == 0) { // 客户端断开
        handleClose();
    }
    else {
        // LT 模式下仍有数据时会再次通知；暂时不可读或被信号中断不关闭连接。
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK || savedErrno == EINTR)
            return;
        // 读取可能已消费 socket 的待处理错误，直接使用本次 readFd 保存的错误码。
        NETLIB_LOG_ERROR << "TcpConnection::handleRead [" << name_
                         << "] fd=" << channel_->fd()
                         << " errno=" << savedErrno;
        handleClose();
    }
}

inline void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting())
        return;

    if (outputBuf_.readableBytes() > 0) {
        ssize_t n;
        do {
            n = ::send(channel_->fd(), outputBuf_.peek(), outputBuf_.readableBytes(),
                       MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            const int savedErrno = errno;
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)
                return;
            NETLIB_LOG_ERROR << "TcpConnection::handleWrite [" << name_
                             << "] fd=" << channel_->fd() << " errno=" << savedErrno;
            handleClose();
            return;
        }
        outputBuf_.retrieve(static_cast<size_t>(n));
        if (outputBuf_.readableBytes() > 0)
            return;
    }

    channel_->disableWriting();
    if (writeCompleteCallback_) {
        TcpConnectionPtr connPtr = shared_from_this();
        auto callback = writeCompleteCallback_;
        loop_->queueInLoop([connPtr, callback = std::move(callback)] { callback(connPtr); });
    }
    if (state_ == StateE::kDisconnecting)
        shutdownInLoop();
}

inline void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == StateE::kDisconnected)
        return;
    NETLIB_LOG_INFO << "TcpConnection::handleClose() "
                    << "fd=" << channel_->fd()
                    << " state=" << enumToStr() << '\n';
    // assert(state_ == StateE::kConnected);
    channel_->disableAll(); // 取消感兴趣事件
    channel_->remove(); // 从 poller 上注销
    setState(StateE::kDisconnected);
    TcpConnectionPtr connPtr(shared_from_this()); // 试着延长对象生命
    // 如果注册过相关回调, 则执行
    if (connectionCallback_)
        connectionCallback_(connPtr);
    if (closeCallback_)
        closeCallback_(connPtr);
}

inline void TcpConnection::handleError() {
    loop_->assertInLoopThread();
    int err = Socket::getSocketError(channel_->fd());
    NETLIB_LOG_ERROR << "TcpConnection::handleError [" << name_
                     << "] - SO_ERROR=" << err << '\n';
}

// 连接建立
inline void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    NETLIB_ASSERT(state_ == StateE::kConnecting);
    setState(StateE::kConnected);
    // TcpConnection 绑定 channel_ 对象, channel_ 开始观察 TcpConnection 的死活
    channel_->tie(shared_from_this());
    channel_->enableReading();

    if (connectionCallback_)
        connectionCallback_(shared_from_this()); // 连接建立了, 执行回调
}

// 连接断开
inline void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    // 下线回调可能释放调用方的最后一个引用，必须持有到 Channel 移除结束。
    TcpConnectionPtr connPtr = shared_from_this();
    const StateE oldState = state_.exchange(StateE::kDisconnected);
    if (oldState == StateE::kConnected || oldState == StateE::kDisconnecting) {
        channel_->disableAll(); // 将 channel_ 感兴趣的事件从 poller 中删除
        if (connectionCallback_)
            connectionCallback_(connPtr);
    }
    channel_->remove(); // 将 channel_ 从 poller 中删除
}

// send 函数的内聚入口
inline void TcpConnection::send(const void *message, size_t len) {
    if (len == 0 || state_ != StateE::kConnected)
        return;
    NETLIB_ASSERT(message != nullptr);
    if (loop_->isInLoopTread()) {
        sendInLoop(message, len);
    } else {
        TcpConnectionPtr connPtr = shared_from_this();
        std::string copy(static_cast<const char*>(message), len);
        loop_->runInLoop([connPtr, message = std::move(copy)] { connPtr->sendInLoop(message); });
    }
}

inline void TcpConnection::send(const std::string &message) {
    send(message.data(), message.size());
}

inline void TcpConnection::send(Buffer *buf) {
    NETLIB_ASSERT(buf != nullptr);
    if (state_ != StateE::kConnected)
        return;
    send(buf->peek(), buf->readableBytes());
    buf->retrieveAll();
}

inline void TcpConnection::sendInLoop(const std::string &message) {
    sendInLoop(message.data(), message.size());
}

// 发送数据, 应用写的快, 而内核发送数据慢, 需要将待写数据写入缓冲区, 并且设置水位回调
inline void TcpConnection::sendInLoop(const void *message, size_t len) {
    loop_->assertInLoopThread();
    // 排队期间可能已经关闭或开始 shutdown，此时不再接收新发送请求。
    if (state_ != StateE::kConnected || len == 0)
        return;
    NETLIB_ASSERT(message != nullptr);
    const char* data = static_cast<const char*>(message);
    size_t written = 0;
    size_t remaining = len;

    // channel_ 第一次开始写数据, 且缓冲区没有待数据发送
    if (!channel_->isWriting() && outputBuf_.readableBytes() == 0) {
        ssize_t n;
        do {
            n = ::send(channel_->fd(), data, len, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n >= 0) {
            written = static_cast<size_t>(n);
            remaining -= written;
            if (remaining == 0 && writeCompleteCallback_) {
                TcpConnectionPtr connPtr = shared_from_this();
                auto callback = writeCompleteCallback_;
                loop_->queueInLoop([connPtr, callback = std::move(callback)] { callback(connPtr); });
            }
        } else {
            const int savedErrno = errno;
            if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
                NETLIB_LOG_ERROR << "TcpConnection::sendInLoop [" << name_
                                 << "] fd=" << channel_->fd() << " errno=" << savedErrno;
                handleClose();
                return;
            }
        }
    }

    // 后续发送追加在已有数据之后，保留部分写入或 EAGAIN 时尚未发送的全部字节。
    if (remaining > 0) {
        const size_t oldLen = outputBuf_.readableBytes();
        outputBuf_.append(data + written, remaining);
        const size_t newLen = outputBuf_.readableBytes();
        if (!channel_->isWriting())
            channel_->enableWriting();
        if (oldLen < highWaterMark_ && newLen >= highWaterMark_ && highWaterMarkCallback_) {
            TcpConnectionPtr connPtr = shared_from_this();
            auto callback = highWaterMarkCallback_;
            loop_->queueInLoop([connPtr, callback = std::move(callback), newLen] {
                callback(connPtr, newLen);
            });
        }
    }
}

inline void TcpConnection::shutdown() {
    StateE expected = StateE::kConnected;
    // 不能把其他线程已设置的 Disconnected 覆盖回 Disconnecting。
    if (state_.compare_exchange_strong(expected, StateE::kDisconnecting)) {
        if (loop_->isInLoopTread()) {
            shutdownInLoop();
        } else {
            TcpConnectionPtr connPtr = shared_from_this();
            loop_->runInLoop([connPtr]
                             { connPtr->shutdownInLoop(); });
        }
    }
}

inline void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    // 排队期间可能已断开；续发完成与排队的 shutdown 也可能先后到达这里。
    if (state_ == StateE::kDisconnecting && !writeShutdown_ &&
        !channel_->isWriting() && outputBuf_.readableBytes() == 0) {
        writeShutdown_ = true;
        socket_->shutdownWrite();
    }
}

inline void TcpConnection::forceClose() {
    StateE expected = StateE::kConnected;
    if (state_.compare_exchange_strong(expected, StateE::kDisconnecting) ||
        expected == StateE::kDisconnecting) {
        TcpConnectionPtr connPtr = shared_from_this();
        // 即使在 loop 线程中也排队，避免移除本批尚未分发的其他活跃 Channel。
        loop_->queueInLoop([connPtr] { connPtr->forceCloseInLoop(); });
    }
}

inline void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == StateE::kConnected || state_ == StateE::kDisconnecting)
        handleClose();
}

}; // netlib

#endif // NETLIB_TCPCONNECTION_H
