#ifndef NETLIB_TCPCONNECTION_H
#define NETLIB_TCPCONNECTION_H

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <functional>
#include <cassert>
#include <string>
#include <iostream>

#include "netlib/eventloop.h"
#include "netlib/inetAddress.h"
#include "netlib/buffer.h"

namespace netlib{
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {

public:
    // using alias
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr &, Buffer &)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr &)>;

    TcpConnection(EventLoop *loop, int fd, uint16_t port) : 
        loop_(loop), state_(StateE::kConnecting), fd_(fd), 
        channel_(std::make_unique<Channel>(loop, fd)), 
        localAddr_(port) {
        channel_->setReadCallback([this]()
                                  { handleRead(); });
        channel_->setWriteCallback([this]()
                                   { handleWrite(); });
        channel_->setCloseCallback([this]()
                                   { handleClose(); });
        channel_->setErrorCallback([this]()
                                   { handleError(); });
    }
    ~TcpConnection() {
        channel_->remove();
        close(fd_);
    }

    // the setter
    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setCloseCallback(CloseCallback cb) {
        closeCallback_ = std::move(cb); 
    }

    // connection ctl
    void connectEstablished() {
        setState(StateE::kConnected);
        channel_->enableReading();
        if (connectionCallback_)
            connectionCallback_(shared_from_this());
    }
    void connectDestroyed();

    // void send(const void *message, size_t len);
    void send(const std::string &message);

    void shutdown();
    void forceClose() { shutdown(); }

private:
    void shutdownInLoop();
    void sendInLoop(const std::string &message);

    enum class StateE {
        kConnecting,
        kConnected,
        kDisconnected,
        kDisconnecting
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
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    // variable
    EventLoop *loop_;
    StateE state_;

    int fd_;
    std::unique_ptr<Channel> channel_;
    InetAddress localAddr_;

    // the Buffer
    Buffer inputBuf_;
    Buffer outputBuf_;
    // Callback
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
};

inline void TcpConnection::handleRead() {
    auto n = inputBuf_.readFd(channel_->fd());
    // assert(n != 0);
    if (n > 0) {
        if (messageCallback_)
            messageCallback_(shared_from_this(), inputBuf_);
    }
    else if (n == 0)
    {
        handleClose();
    }
    else {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        std::cout << "TcpConnection::handleRead" << std::endl;
        handleError();
    }
}

inline void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (channel_->isWriting()) {
        auto n = ::write(channel_->fd(),
                         outputBuf_.peek(),
                         outputBuf_.readableBytes());
        
        if (n > 0) {
            outputBuf_.retrieve(n);
            if (outputBuf_.readableBytes() == 0) {
                channel_->disableWriting();
                if (state_ == StateE::kDisconnecting)
                    shutdownInLoop();
            } else {
                std::cout << "outputBuf will continue to write" << std::endl;
            }
        } else {
            std::cerr << "TcpConnection::handleWrite" << std::endl;
        }
    } else {
        std::cout << "Connection is shutdown, no more writing" << std::endl;
    }
}

inline void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    std::cout << "TcpConnection::handleClose state =  "<< enumToStr() << std::endl;
    // assert(state_ == StateE::kConnected);
    channel_->disableAll();
    close(fd_);
    setState(StateE::kDisconnected);
    if (closeCallback_)
        closeCallback_(shared_from_this());
}

inline void TcpConnection::handleError() {
    std::cout << "TcpConnection::handleError" << std::endl;
    handleClose();
}

inline void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    assert(state_ == StateE::kConnected);
    setState(StateE::kDisconnected);
    channel_->disableAll();
    if (connectionCallback_)
        connectionCallback_(shared_from_this());
    channel_->remove();
}

inline void TcpConnection::send(const std::string &message) {
    if (state_ == StateE::kConnected) {
        if (loop_->isInLoopTread()){
            sendInLoop(message);
        } else {
            // TODO->runInLoop
        }
    }
}

inline void TcpConnection::sendInLoop(const std::string &message) {
    loop_->assertInLoopThread();
    ssize_t nwrote = 0;
    if (!channel_->isWriting() && outputBuf_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), message.data(), message.size());

        if (nwrote >= 0) {
            if (static_cast<size_t>(nwrote) < message.size()) {
                std::cout << "output Buffer will continue to write" << std::endl;
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                std::cout << "TcpConnection::sendInLoop" << std::endl;
            }
        }
    }

    assert(nwrote >= 0);
    if (static_cast<size_t>(nwrote) < message.size()) {
        outputBuf_.append(message.data() + nwrote, message.size() - nwrote);
        if (!channel_->isWriting())
            channel_->enableWriting();
    }
}

inline void TcpConnection::shutdown() {
    if (state_ == StateE::kConnected) {
        setState(StateE::kDisconnecting);
        if (loop_->isInLoopTread()) {
            shutdownInLoop();
        } else {
            // TODO->runInLoop
        }
    }
}

inline void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    // setState(StateE::kDisconnected);
    ::shutdown(channel_->fd(), SHUT_WR);
    if (!channel_->isWriting())
        channel_->disableAll();
}

}; // netlib

#endif // NETLIB_TCPCONNECTION_H