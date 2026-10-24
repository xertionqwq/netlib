#ifndef NETLIB_SOCKET_H
#define NETLIB_SOCKET_H

#include <cerrno>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "netlib/noncopyable.h"
#include "netlib/log.h"
#include "netlib/inetAddress.h"

namespace netlib {

// 独占传入的有效 socket fd；外部不得再次 close，由析构负责回收。
class Socket : private Noncopyable{

public:
    explicit Socket(int sockfd) : sockfd_(sockfd) {
        NETLIB_ASSERT(sockfd_ >= 0);
    }
    ~Socket() {
        ::close(sockfd_);
    }

    int fd() const { return sockfd_; }
    // 绑定 ip 与 port
    void bindAddress(const InetAddress &localaddr);
    // 开始监听
    void listen();

    // peeraddr 必须非空；成功返回的 fd 由调用方接管，并设置 NONBLOCK/CLOEXEC。
    // 失败返回 -1，保留 errno，且不修改 peeraddr；由调用方区分 EAGAIN/EINTR 等错误。
    int accept(InetAddress *peeraddr);

    void shutdownWrite();

    static int getSocketError(int sockfd);

    // 选项设置失败时记录日志并保留 errno，保持现有 void 接口。
    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

private:
    void setSocketOption(int level, int option, bool on, const char* optionName);

    const int sockfd_;
};

inline void Socket::bindAddress(const InetAddress &localaddr) {
    const int ret = ::bind(sockfd_, reinterpret_cast<const sockaddr*>(&localaddr.addr()),
                           sizeof(localaddr.addr()));
    if (ret < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_FATAL << "bind fd=" << sockfd_ << " errno=" << savedErrno;
    }
}

inline void Socket::listen() {
    const int ret = ::listen(sockfd_, 1024);
    if (ret < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_FATAL << "listen fd=" << sockfd_ << " errno=" << savedErrno;
    }
}

inline int Socket::accept(InetAddress *peeraddr) {
    NETLIB_ASSERT(peeraddr != nullptr);
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    const int connfd = ::accept4(sockfd_, reinterpret_cast<sockaddr*>(&addr), &len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) {
        peeraddr->setSockAddrInet(addr); // 将新连接 ip 和 port 打包
    }
    return connfd; // 返回新连接的 fd
}

inline void Socket::shutdownWrite() {
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_ERROR << "shutdownWrite fd=" << sockfd_ << " errno=" << savedErrno;
    }
}

inline int Socket::getSocketError(int sockfd) {
    int optval = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof optval);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
        return errno;
    else
        return optval;
}

inline void Socket::setTcpNoDelay(bool on) {
    setSocketOption(IPPROTO_TCP, TCP_NODELAY, on, "TCP_NODELAY");
}

inline void Socket::setReuseAddr(bool on) {
    setSocketOption(SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR");
}

inline void Socket::setReusePort(bool on) {
    setSocketOption(SOL_SOCKET, SO_REUSEPORT, on, "SO_REUSEPORT");
}

inline void Socket::setKeepAlive(bool on) {
    setSocketOption(SOL_SOCKET, SO_KEEPALIVE, on, "SO_KEEPALIVE");
}

inline void Socket::setSocketOption(int level, int option, bool on, const char* optionName) {
    const int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, level, option, &optval, sizeof optval) < 0) {
        const int savedErrno = errno;
        NETLIB_LOG_ERROR << "setsockopt fd=" << sockfd_ << " option=" << optionName
                         << " errno=" << savedErrno;
    }
}
} // netlib

#endif // NETLIB_SOCKET_H
