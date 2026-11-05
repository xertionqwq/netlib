#include <netlib/tcpServer.h>
#include <netlib/eventloopthread.h>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

static_assert(__cplusplus >= 201703L, "netlib must propagate C++17");

netlib::EventLoop* peerCurrentLoop();
std::string peerBuffer();

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        require(peerCurrentLoop() == nullptr, "unexpected initial EventLoop");
        netlib::EventLoop base;
        require(peerCurrentLoop() == &base, "EventLoop state differs between translation units");
        require(peerBuffer() == std::string("a\0b", 3), "binary Buffer data changed");

        std::promise<bool> completed;
        auto ready = completed.get_future();
        {
            netlib::EventLoopThread worker;
            auto* loop = worker.startLoop();
            loop->queueInLoop([loop, &completed] {
                loop->assertInLoopThread();
                completed.set_value(peerCurrentLoop() == loop);
            });
            require(ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                    "worker task timed out");
            require(ready.get(), "worker EventLoop state differs between translation units");
        }
        require(peerCurrentLoop() == &base, "worker changed the control thread EventLoop");
        std::cout << "installed netlib: C++17, multi-TU, Buffer and worker task passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "installed netlib: " << error.what() << '\n';
        return 1;
    }
}
