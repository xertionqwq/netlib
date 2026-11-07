#include "echo_server.h"

// 简易持续回声：nc 127.0.0.1 12300，Ctrl+C 正常退出。
int main(int argc, char* argv[]) {
    using namespace netlib;
    using namespace netlib::example;
    netlib::Logger::setLevel(netlib::LogLevel::WARN); // 压测版本：关闭热路径 INFO/DEBUG 日志。
    EchoOptions options;
    try {
        options = parseEchoOptions(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        printEchoUsage(std::cerr);
        return 2;
    }
    if (options.help) {
        printEchoUsage(std::cout);
        return 0;
    }
    try {
        {
            EventLoop loop;
            SignalStop stop(&loop); // 先屏蔽信号，再创建 TcpServer 工作线程。
            EchoServer server(&loop, InetAddress(options.port), "EchoServer-01", options.threads);
            server.start();
            server.printReady(std::cout); // 已完成监听及工作线程初始化，立即刷新。
            loop.loop();
            // 返回事件分发之外，再依次清理 Server、信号 Channel 和 EventLoop。
        }
        std::cout << "STOPPED" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "echo_server: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
