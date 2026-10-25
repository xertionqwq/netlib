#ifndef NETLIB_TCPSERVER_H
#define NETLIB_TCPSERVER_H

#include <functional>
#include <future>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "netlib/Callbacks.h"
#include "netlib/acceptor.h"
#include "netlib/eventloop.h"
#include "netlib/eventloopthreadpool.h"
#include "netlib/tcpConnection.h"
#include "netlib/inetAddress.h"
#include "netlib/noncopyable.h"

namespace netlib {

// 构造、配置、启动与析构均在 baseLoop 线程执行。
// 在事件分发之外析构；回调不得同步等待正在析构的 baseLoop，工作 loop 不得自行退出。
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
    void setThreadInitCallback(ThreadInitCallback cb) {
        checkConfigurable();
        threadInitCallback_ = std::move(cb);
    }
    void setConnectionCallback(ConnectionCallback cb) {
        checkConfigurable();
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        checkConfigurable();
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        checkConfigurable();
        writeCompleteCallback_ = std::move(cb);
    }

    // 设置底层 subLoop 的个数
    void setThreadNum(int numThreads);

    // 开启服务器监听
    void start();

private:
    void checkConfigurable() const {
        loop_->assertInLoopThread();
        if (startAttempted_)
            throw std::logic_error("Cannot configure TcpServer after startup was attempted");
    }
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
    bool startAttempted_; // 以下启动状态仅在 baseLoop 线程访问
    bool started_;
    std::shared_ptr<std::atomic_bool> alive_; // 跨线程任务的存活标志，不延长 Server 本身的生命周期

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
    , acceptor_(std::make_unique<Acceptor>(loop, listenAddr, option == Option::kReusePort))
    , threadPool_(std::make_shared<EventLoopThreadPool>(loop, name_)) // 只有调用 start() 时才会开启线程
    , messageCallback_([](const TcpConnectionPtr&, Buffer* buffer, Timestamp) {
        buffer->retrieveAll();
    })
    , startAttempted_(false)
    , started_(false)
    , alive_(std::make_shared<std::atomic_bool>(true))
    , nextConnId_(1)
{
    // 设置处理新连接 fd 的回调
    acceptor_->setNewConnectionCallback([this](int sockfd, const InetAddress &peerAddr)
                                        { newConnection(sockfd, peerAddr); });
}

inline TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    NETLIB_LOG_INFO << "TcpServer::~TcpServer [" << name_
                    << " ] destructing\n";

    alive_->store(false);
    acceptor_.reset(); // 先停止接入，不能等成员自动析构时才关闭监听。
    auto connections = std::move(connections_);
    for (const auto& item : connections) {
        if (item.second->getLoop() == loop_) {
            item.second->setCloseCallback({});
            item.second->connectDestroyed();
        }
    }

    std::vector<std::future<void>> completions;
    if (threadPool_ && threadPool_->started()) {
        // 即使连接已移表，也要等待该工作 loop 中此前排队的清理任务。
        for (EventLoop* ioLoop : threadPool_->getAllLoops()) {
            if (ioLoop == loop_)
                continue;
            std::vector<TcpConnectionPtr> owned;
            for (const auto& item : connections) {
                if (item.second->getLoop() == ioLoop)
                    owned.push_back(item.second);
            }
            auto done = std::make_shared<std::promise<void>>();
            completions.push_back(done->get_future());
            ioLoop->queueInLoop([ioLoop, owned = std::move(owned), done] {
                try {
                    for (const auto& conn : owned) {
                        conn->setCloseCallback({});
                        conn->connectDestroyed();
                    }
                    // 再经过一轮任务队列，确保本批回调及其连接引用已释放。
                    ioLoop->queueInLoop([done] { done->set_value(); });
                } catch (...) {
                    done->set_exception(std::current_exception());
                }
            });
        }
    }
    for (auto& completion : completions)
        completion.get();
    connections.clear();
    threadPool_.reset(); // 清理确认完成后才允许工作循环退出。
}

inline void TcpServer::setThreadNum(int numThreads) {
    checkConfigurable();
    threadPool_->setThreadNum(numThreads);
}

// loop.loop() 留给用户调用
inline void TcpServer::start() {
    loop_->assertInLoopThread();
    if (startAttempted_)
        throw std::logic_error("TcpServer can only be started once");
    startAttempted_ = true;
    try {
        threadPool_->start(threadInitCallback_);
        acceptor_->listen();
        started_ = true;
    } catch (...) {
        threadPool_.reset();
        throw;
    }
}

// 有一个新客户的连接, acceptor 就会执行这个连接
inline void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr) {
    loop_->assertInLoopThread();
    std::string connName;
    bool inserted = false;
    try {
        auto socket = Socket::adopt(sockfd); // 此后所有异常路径都由唯一持有者回收 fd。
        if (!started_ || !alive_->load())
            return;
        EventLoop *ioLoop = threadPool_->getNextLoop();
        connName = name_ + ipPort_ + '#' + std::to_string(nextConnId_);
        ++nextConnId_;

        NETLIB_LOG_INFO << "TcpServer::newConnection [" << name_
                        << "] - new connection [" << connName
                        << "] from " << peerAddr.toIpPort() << '\n';

        // 通过 sockfd 获取其绑定的本机的 ip 地址和端口信息
        InetAddress localAddr(Socket::getLocalAddr(sockfd));

        // 根据连接成功的 socktfd, 创建 TcpConnection 对象
        TcpConnectionPtr conn = std::make_shared<TcpConnection>(
                                                ioLoop,
                                                connName,
                                                std::move(socket),
                                                localAddr,
                                                peerAddr
                                            );
        // 回调设置路径 用户->TcpServer->TcpConnection->Channel->Poller notify->Channel 执行回调
        conn->setConnectionCallback(connectionCallback_);
        if (messageCallback_)
            conn->setMessageCallback(messageCallback_);
        else
            conn->setMessageCallback([](const TcpConnectionPtr&, Buffer* b, Timestamp) { b->retrieveAll(); });
        conn->setWriteCompleteCallback(writeCompleteCallback_);
        // 设置如何关闭连接的回调
        const std::weak_ptr<std::atomic_bool> weak = alive_;
        EventLoop* baseLoop = loop_;
        auto onClose = [this, baseLoop, weak](const TcpConnectionPtr& connPtr) {
            // 工作线程不访问 Server；进入 baseLoop 并检查存活状态后才使用 this。
            const std::weak_ptr<TcpConnection> weakConn = connPtr;
            baseLoop->queueInLoop([this, weak, weakConn] {
                auto alive = weak.lock();
                if (alive && alive->load()) {
                    if (auto conn = weakConn.lock())
                        removeConnection(conn);
                }
            });
        };
        conn->setCloseCallback(onClose);
        inserted = connections_.emplace(connName, conn).second;
        NETLIB_ASSERT(inserted);
        ioLoop->queueInLoop([conn, alive = alive_, onClose] {
            if (!alive->load())
                return;
            try {
                conn->connectEstablished();
            } catch (const std::exception& error) {
                // 注册失败时尚未通知上线，清理 Channel 后交给 baseLoop 移表。
                conn->setConnectionCallback({});
                conn->setCloseCallback({});
                conn->connectDestroyed();
                onClose(conn);
                NETLIB_LOG_ERROR << "TcpServer::connectEstablished " << error.what();
            }
        });
    } catch (const std::exception& error) {
        if (inserted)
            connections_.erase(connName);
        NETLIB_LOG_ERROR << "TcpServer::newConnection fd=" << sockfd << " " << error.what();
    } catch (...) {
        if (inserted)
            connections_.erase(connName);
        NETLIB_LOG_ERROR << "TcpServer::newConnection fd=" << sockfd << " unknown error";
    }
}

inline void TcpServer::removeConnection(const TcpConnectionPtr &conn) {
    loop_->assertInLoopThread();
    loop_->runInLoop([this, conn]
                     { removeConnectionInLoop(conn); });
}

inline void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn) {
    loop_->assertInLoopThread();
    NETLIB_LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
                    << "] - connection " << conn->name() << '\n';

    size_t n = connections_.erase(conn->name());
    (void)n; // 消除编译警告
    NETLIB_ASSERT(n == 1);
    EventLoop *ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn]
                        { conn->connectDestroyed(); });
}

} // namespace netlib

#endif // NETLIB_TCPSERVER_H
