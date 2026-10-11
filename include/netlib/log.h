#ifndef NETLIB_LOG_H
#define NETLIB_LOG_H

#include <cassert>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "netlib/timestamp.h"

namespace netlib {

// ============================================================
// Log 级别
// ============================================================
enum class LogLevel {
    DEBUG = 0, // 调试信息
    INFO = 1,  // 普通信息
    WARN = 2,  // 警告信息
    ERROR = 3, // 错误信息
    FATAL = 4  // core信息
};

// ============================================================
// Logger — 流式日志，析构时一次性写入 stderr（线程安全）
// ============================================================
class Logger {
public:
    Logger(LogLevel level, const char* file, int line)
        : level_(level), file_(file), line_(line) {}

    ~Logger() {
        // 宏入口已经决定输出, 不再根据变化后的门槛重新过滤
        sink();
        if (level_ == LogLevel::FATAL) {
            std::fflush(stderr);
            std::abort();
        }
    }

    // 返回流式对象, << msg 接入字符串
    std::ostream& stream() { return os_; }

    // 设置日志级别
    static void setLevel(LogLevel lv) {
        minLevel_.store(lv, std::memory_order_relaxed);
    }
    // 查看日志级别
    static LogLevel getLevel() {
        return minLevel_.load(std::memory_order_relaxed);
    }

    static bool shouldLog(LogLevel lv) {
        return lv == LogLevel::FATAL || lv >= getLevel();
    }

private:
    // 整条日志格式化输出
    void sink() {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        // 得到时间字符串
        const std::string timeText = timestamp_.toString();

        std::ostringstream threadId;
        threadId << threadId_;

        // 颜色（ERROR 红色，FATAL 紫色，WARN 黄色）
        const char* color = "";
        const char* reset = "";
        if (level_ >= LogLevel::FATAL) { color = "\033[35m"; reset = "\033[0m"; }
        else if (level_ >= LogLevel::ERROR) { color = "\033[31m"; reset = "\033[0m"; }
        else if (level_ >= LogLevel::WARN)  { color = "\033[33m"; reset = "\033[0m"; }

        std::fprintf(stderr, "%s%s [%s] [tid=%s] %s:%d %s%s\n",
                     color, levelStr(level_), timeText.c_str(),
                     threadId.str().c_str(),
                     basename(file_), line_,
                     os_.str().c_str(), reset);
    }

    static const char* basename(const char* path) {
        const char* p = path;
        for (const char* c = path; *c; ++c)
            if (*c == '/') p = c + 1;
        return p;
    }

    static const char* levelStr(LogLevel lv) {
        switch (lv) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
        }
        return "?????";
    }

    struct ErrnoGuard {
        int savedErrno = errno;
        ~ErrnoGuard() { errno = savedErrno; }
    };

    // 最先构造、最后析构, 保留整条日志表达式执行前的 errno
    ErrnoGuard errnoGuard_;
    inline static std::atomic<LogLevel> minLevel_{LogLevel::DEBUG};

    Timestamp timestamp_ = Timestamp::now(); // 记录日志对象创建时的时间
    LogLevel level_;        // 日志等级
    const char* file_;      // 源文件
    int line_;
    std::thread::id threadId_ = std::this_thread::get_id();
    std::ostringstream os_; // 日志缓冲区
};

// ============================================================
// 日志宏
// ============================================================
// 完整的 if/else 防止宏与调用处的 else 错误绑定
#define NETLIB_LOG_DEBUG                                           \
    if (!netlib::Logger::shouldLog(netlib::LogLevel::DEBUG)) {}      \
    else netlib::Logger(netlib::LogLevel::DEBUG, __FILE__, __LINE__).stream()
#define NETLIB_LOG_INFO                                            \
    if (!netlib::Logger::shouldLog(netlib::LogLevel::INFO)) {}       \
    else netlib::Logger(netlib::LogLevel::INFO, __FILE__, __LINE__).stream()
#define NETLIB_LOG_WARN                                            \
    if (!netlib::Logger::shouldLog(netlib::LogLevel::WARN)) {}       \
    else netlib::Logger(netlib::LogLevel::WARN, __FILE__, __LINE__).stream()
#define NETLIB_LOG_ERROR                                           \
    if (!netlib::Logger::shouldLog(netlib::LogLevel::ERROR)) {}      \
    else netlib::Logger(netlib::LogLevel::ERROR, __FILE__, __LINE__).stream()
// FATAL 始终输出, 刷新后终止程序
#define NETLIB_LOG_FATAL netlib::Logger(netlib::LogLevel::FATAL, __FILE__, __LINE__).stream()

// ============================================================
// 断言宏 — 始终生效, 不受 NDEBUG 影响, 条件只求值一次
// ============================================================
#define NETLIB_ASSERT(cond)                                   \
    do {                                                      \
        if (!(cond)) {                                        \
            NETLIB_LOG_FATAL << "assertion failed: " #cond;   \
        }                                                     \
    } while (0)

} // namespace netlib

#endif // NETLIB_LOG_H
