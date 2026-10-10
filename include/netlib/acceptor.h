#ifndef NETLIB_ACCEPTOR_H
#define NETLIB_ACCEPTOR_H

#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <functional>

#include "netlib/inetAddress.h"
#include "netlib/eventloop.h"

namespace netlib{

class Acceptor{
    /*
    ** own acceptFd, to accept new fd, pass to TcpConnection
    ** noncopyable
    */

    Acceptor(const Acceptor &others) = delete;
    Acceptor &operator=(const Acceptor &others) = delete;

public:
    // using alias
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress &)>;

    // constructor
    Acceptor(EventLoop *loop, const InetAddress &listenAddr);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnectionCallback_ = std::move(cb);
    }

    bool listening() const { return listening_; }

    static int createNonblockingOrDie();

private:
    void handleRead();

    // variable
    EventLoop *loop_;
    int acceptFd_;
    std::unique_ptr<Channel> acceptChannel_;      // 掌管监听 fd 的生命周期
    NewConnectionCallback newConnectionCallback_; // 连接建立时负责执行的回调
    bool listening_;
};
inline Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr) :
                loop_(loop), 
                acceptFd_(Acceptor::createNonblockingOrDie()), 
                listening_(false) {
    assert(acceptFd_ >= 0);
    int opt = 1;
    setsockopt(acceptFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址和端口
    int ret = ::bind(acceptFd_, (sockaddr*)&(listenAddr.addr()), sizeof(listenAddr.addr()));
    if (ret == -1) {
        std::cerr << "Acceptor bind failed: " << strerror(errno) << std::endl;
        assert(false);
    }
    // 设置监听
    ret = ::listen(acceptFd_, 128);
    if (ret == -1) {
        std::cerr << "Acceptor listen failed: " << strerror(errno) << std::endl;
        assert(false);
    }
    fcntl(acceptFd_, F_SETFL, O_NONBLOCK);

    // 为监听 fd 创建 Channel 
    acceptChannel_ = std::make_unique<Channel>(loop, acceptFd_);
    // 设置读回调为新建连接 fd
    acceptChannel_->setReadCallback([this]()
                                    { handleRead(); });
    acceptChannel_->enableReading(); // 向 epoll 注册读事件
}

inline Acceptor::~Acceptor() {
    loop_->assertInLoopThread();
    close(acceptFd_);
}

inline int Acceptor::createNonblockingOrDie() {
    // 创建监听 fd
    int sockfd = ::socket(AF_INET,
                          SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
    if (sockfd < 0) {
        std::cerr << "Acceptor::createNonblockingOrDie" << std::endl;
        assert(false);
    }

    return sockfd;
}

inline void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    InetAddress connAddr(0);
    // 根据监听 fd 创建连接 fd
    int connfd = ::accept(acceptFd_, connAddr.sockaddrPtr(), connAddr.sizePtr());
    fcntl(connfd, F_SETFL, O_NONBLOCK);

    if (connfd >= 0) {
        // 连接成功, 进入连接回调
        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, connAddr);
        }
    } else {
        std::cerr << "Acceptor::handleRead accept:" << std::endl;
    }
}

}; // netlib

#endif // NETLIB_ACCEPTOR_H