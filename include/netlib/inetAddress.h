#ifndef NETLIB_INETADDRESS_H
#define NETLIB_INETADDRESS_H

#include <arpa/inet.h>
#include <string>
#include <stdexcept>

namespace netlib{

// 封装 socket 地址类型
class InetAddress{
    /*
    ** Encapsulation of sockaddr
    ** copyable
    */

public:
    // for server, to listen port
    explicit InetAddress(uint16_t port) : addr_({}) {
        // 设置 ipv4, 地址, 端口
        addr_.sin_family = AF_INET;
        addr_.sin_addr.s_addr = ::htonl(INADDR_ANY);
        addr_.sin_port = ::htons(port);
        size_ = sizeof(addr_);
    }
    // for client, to bind ip and port
    InetAddress(const std::string &ip, uint16_t port) : addr_({}) {
        addr_.sin_family = AF_INET;
        addr_.sin_port = ::htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1)
            throw std::invalid_argument("invalid IPv4 address: " + ip);
        size_ = sizeof(addr_);
    }
    InetAddress(sockaddr_in other) {
        addr_ = other;
        size_ = sizeof(addr_);
    }

    // for vairable
    const sockaddr_in &addr() const { return addr_; }
    sockaddr* sockaddrPtr() {
        return reinterpret_cast<struct sockaddr *>(&addr_);
    }

    void setSockAddrInet(const sockaddr_in &addr) { addr_ = addr; }

    // 返回大小
    socklen_t size() const { return size_; }
    socklen_t *sizePtr() {
        return &size_;
    }
    // 返回端口
    uint16_t port() const { return ::ntohs(addr_.sin_port); }
    // 返回 ip 地址
    std::string ip() const {
        char buf[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf) == nullptr)
            return {};
        return buf;
    }
    std::string toIpPort() const {
        std::string curIp = ip();
        return curIp + ':' + std::to_string(port());
    }

private:
    struct sockaddr_in addr_;
    socklen_t size_;
};
}; // netlib

#endif // NETLIB_INETADDRESS_H
