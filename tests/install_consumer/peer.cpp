#include <netlib/tcpServer.h>
#include <netlib/eventloopthread.h>

#include <string>

netlib::EventLoop* peerCurrentLoop() {
    return netlib::loopPtr;
}

std::string peerBuffer() {
    netlib::Buffer buffer;
    buffer.append("a\0b", 3);
    return buffer.retrieveAllAsString();
}
