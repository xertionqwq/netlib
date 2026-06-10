#ifndef NETLIB_IETADDRESS_H
#define NETLIB_IETADDRESS_H

#include <arpa/inet.h>
#include <string>

namespace netlib{
class InetAddress{
    /*
    ** Encapsulation of sockaddr
    ** copyable
    */

public:
    // for server, to listen port
    explicit InetAddress(uint16_t port) : addr_({}) {
        addr_.sin_family = AF_INET;
        addr_.sin_addr.s_addr = ::htonl(INADDR_ANY);
        addr_.sin_port = ::htons(port);
        size_ = sizeof(addr_);
    }
    // for client, to bind ip and port
    InetAddress(const std::string &ip, uint16_t port) : addr_({}) {
        addr_.sin_family = AF_INET;
        addr_.sin_port = ::htons(port);
        ::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
        size_ = sizeof(addr_);
    }

    // for vairable
    const sockaddr_in &addr() const { return addr_; }
    sockaddr* sockaddrPtr() {
        return reinterpret_cast<struct sockaddr *>(&addr_);
    }
    socklen_t size() const { return size_; }
    socklen_t *sizePtr() {
        return &size_;
    }
    uint16_t port() const { return ::ntohs(addr_.sin_port); }
    std::string ip() const {
        std::string buf;
        ::inet_ntop(AF_INET, &addr_.sin_addr, buf.data(), buf.size());
        return buf;
    }

private:
    struct sockaddr_in addr_;
    socklen_t size_;
};
}; // netlib

#endif // NETLIB_IETADDRESS_H