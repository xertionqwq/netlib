#ifndef NETLIB_LOG_H
#define NETLIB_LOG_H

#include <cassert>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace netlib {

// ============================================================
// Log 级别
// ============================================================
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4 };

// ============================================================
// Logger — 流式日志，析构时一次性写入 stderr（线程安全）
// ============================================================
class Logger {
public:
    Logger(LogLevel level, const char* file, int line)
        : level_(level), file_(file), line_(line) {}

    ~Logger() {
        if (level_ < getLevel()) return;
        sink();
    }

    std::ostream& stream() { return os_; }

    static void setLevel(LogLevel lv) { getLevel() = lv; }
    static LogLevel& getLevel() {
        static LogLevel s_level = LogLevel::DEBUG;
        return s_level;
    }

private:
    void sink() {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        // 时间戳
        auto now = std::time(nullptr);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&now));

        // 颜色（仅 ERROR/FATAL 红色，WARN 黄色）
        const char* color = "";
        const char* reset = "";
        if (level_ >= LogLevel::FATAL) { color = "\033[35m"; reset = "\033[0m"; }
        else if (level_ >= LogLevel::ERROR) { color = "\033[31m"; reset = "\033[0m"; }
        else if (level_ >= LogLevel::WARN)  { color = "\033[33m"; reset = "\033[0m"; }

        std::fprintf(stderr, "%s%s [%s] %s:%d %s%s\n",
                     color, levelStr(level_), timeBuf,
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

    LogLevel level_;
    const char* file_;
    int line_;
    std::ostringstream os_;
};

// ============================================================
// 日志宏
// ============================================================
#define NETLIB_LOG_DEBUG netlib::Logger(netlib::LogLevel::DEBUG, __FILE__, __LINE__).stream()
#define NETLIB_LOG_INFO  netlib::Logger(netlib::LogLevel::INFO,  __FILE__, __LINE__).stream()
#define NETLIB_LOG_WARN  netlib::Logger(netlib::LogLevel::WARN,  __FILE__, __LINE__).stream()
#define NETLIB_LOG_ERROR netlib::Logger(netlib::LogLevel::ERROR, __FILE__, __LINE__).stream()
#define NETLIB_LOG_FATAL netlib::Logger(netlib::LogLevel::FATAL, __FILE__, __LINE__).stream()

// ============================================================
// 断言宏
// ============================================================
#define NETLIB_ASSERT(cond)                                   \
    do {                                                      \
        if (!(cond)) {                                        \
            NETLIB_LOG_FATAL << "assertion failed: " #cond;   \
            assert(cond);                                     \
        }                                                     \
    } while (0)

} // namespace netlib

#endif // NETLIB_LOG_H
