#ifndef NETLIB_TCPSERVER_H
#define NETLIB_TCPSERVER_H

#include "functional"
#include "binders.h"
#include "string"
#include "memory"
#include "atomic"
#include "unordered_map"

#include "netlib/Callbacks.h"
#include "netlib/acceptor.h"
#include "netlib/eventloop.h"
#include "netlib/eventloopthreadpool.h"
#include "netlib/inetAddress.h"
#include "netlib/noncopyable.h"

namespace netlib {

// 连接服务器对外使用的类
class TcpServer : private Noncopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    // 枚举, 是否复用端口
    enum class Option {
        kNoReusePort,
        kReusePort
    };

    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = Option::kNoReusePort);
    ~TcpServer();

    const std::string &ipPort() const { return ipPort_; }
    const std::string &name() const { return name_; }
    EventLoop *getLoop() const { return loop_; }

    // 设置相应回调
    void setThreadInitCallback(ThreadInitCallback cb) { threadInitCallback_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); }

    // 设置底层 subLoop 的个数
    void setThreadNum(int numThreads);

    // 开启服务器监听
    void start();

private:
    /*
    ** 接受新连接与断开连接
    */
    // 根据轮询算法选择并唤醒一个 subLoop, 把当前 connfd 封装为 channel 并分发给 subloop
    void newConnection(int sockfd, const InetAddress &peerAddr);
    void removeConnection(const TcpConnectionPtr &conn);
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

private:
    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    EventLoop *loop_; // baseLoop
    const std::string ipPort_;
    const std::string name_;

    std::unique_ptr<Acceptor> acceptor_; // 运行在 mainLoop 中, 监听连接事件
    std::shared_ptr<EventLoopThreadPool> threadPool_; // one loop per thread

    ConnectionCallback connectionCallback_;       // 有新连接时的回调
    MessageCallback messageCallback_;             // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完以后的回调

    ThreadInitCallback threadInitCallback_; // loop 线程初始化的回调
    std::atomic_bool started_;

    std::size_t nextConnId_;    // 连接计数
    ConnectionMap connections_; // 保存所有连接
};

inline TcpServer::TcpServer(EventLoop *loop,
                            const InetAddress &listenAddr,
                            const std::string &nameArg,
                            Option option)
    : loop_(checkLoopInThread(loop)) // 创建资源前检查主 loop 非空及所属线程
    , ipPort_(listenAddr.toIpPort())
    , name_(nameArg)
    , acceptor_(std::make_unique<Acceptor>(loop, listenAddr, option))
    , threadPool_(std::make_shared<EventLoopThreadPool>(loop, name_)) // 只有调用 start() 时才会开启线程
    , connectionCallback_() // TODO 设置默认回调
    , messageCallback_() // TODO 设置默认回调
    , writeCompleteCallback_() // TODO 设置默认回调
    , started_(false)
    , nextConnId_(1)
{
    // 设置处理新连接 fd 的回调
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                                  std::placeholders::_1, std::placeholders::_2));
}

inline TcpServer::~TcpServer() {

}

inline void TcpServer::setThreadNum(int numThreads) {
    assert(numThreads >= 0);
    assert(!started_); // 仅能在未启动前设置
    threadPool_->setThreadNum(numThreads);
}

// loop.loop() 留给用户调用
inline void TcpServer::start() {
    assert(started_ == false); // 防止该对象多次start
    threadPool_->start(); // 启动线程池
    loop_->runInLoop([this] // mainLoop 开始监听
                     { acceptor_->listen(); });
    started_ = true;
}

inline void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr) {

}

inline void TcpServer::removeConnection(const TcpConnectionPtr &conn) {

}

inline void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn) {

}

} // namespace netlib

#endif // NETLIB_TCPSERVER_H
