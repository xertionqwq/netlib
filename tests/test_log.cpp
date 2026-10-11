#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netlib/log.h"
#include "netlib/noncopyable.h"

// 不使用标准 assert, 确保 NDEBUG 构建也会检查测试结果
static void check(bool condition, const char* message) {
    if (!condition) {
        std::cout << "test_log failed: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// 临时重定向 stderr, 检查实际输出而不是只检查程序是否崩溃
class StderrCapture : private netlib::Noncopyable {
public:
    StderrCapture() {
        std::fflush(stderr);
        file_ = std::tmpfile();
        check(file_ != nullptr, "tmpfile failed");
        savedFd_ = ::dup(STDERR_FILENO);
        check(savedFd_ >= 0, "dup failed");
        check(::dup2(::fileno(file_), STDERR_FILENO) >= 0, "dup2 failed");
    }

    ~StderrCapture() {
        std::fflush(stderr);
        ::dup2(savedFd_, STDERR_FILENO);
        ::close(savedFd_);
        std::fclose(file_);
    }

    std::string output() {
        std::fflush(stderr);
        check(std::fseek(file_, 0, SEEK_SET) == 0, "fseek failed");
        std::string result;
        char buffer[4096];
        size_t count;
        while ((count = std::fread(buffer, 1, sizeof buffer, file_)) > 0)
            result.append(buffer, count);
        check(!std::ferror(file_), "fread failed");
        return result;
    }

private:
    std::FILE* file_ = nullptr;
    int savedFd_ = -1;
};

void test_log_basic() {
    StderrCapture capture;
    const int line = __LINE__ + 1;
    NETLIB_LOG_INFO << "basic message " << 42;
    NETLIB_LOG_DEBUG << "debug message";
    NETLIB_LOG_WARN << "warning message";
    NETLIB_LOG_ERROR << "error message";

    const std::string output = capture.output();
    check(output.find("basic message 42") != std::string::npos, "streamed message missing");
    check(output.find("debug message") != std::string::npos, "DEBUG message missing");
    check(output.find("warning message") != std::string::npos, "WARN message missing");
    check(output.find("error message") != std::string::npos, "ERROR message missing");
    check(output.find("test_log.cpp:" + std::to_string(line)) != std::string::npos,
          "source file or line missing");
    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    check(output.find("[tid=" + threadId.str() + "]") != std::string::npos,
          "originating thread ID missing");
    check(output.find("INFO  [") != std::string::npos, "level or timestamp missing");
}

void test_log_level_filter() {
    StderrCapture capture;
    netlib::Logger::setLevel(netlib::LogLevel::WARN);
    check(netlib::Logger::getLevel() == netlib::LogLevel::WARN, "level update failed");
    int evaluated = 0;
    NETLIB_LOG_DEBUG << "filtered-debug " << ++evaluated;
    NETLIB_LOG_INFO << "filtered-info " << ++evaluated;
    NETLIB_LOG_WARN << "allowed-warning " << ++evaluated;
    NETLIB_LOG_ERROR << "allowed-error " << ++evaluated;
    check(evaluated == 2, "filtered log arguments were evaluated");

    netlib::Logger::setLevel(netlib::LogLevel::FATAL);
    NETLIB_LOG_DEBUG << ++evaluated;
    NETLIB_LOG_INFO << ++evaluated;
    NETLIB_LOG_WARN << ++evaluated;
    NETLIB_LOG_ERROR << ++evaluated;
    check(evaluated == 2, "FATAL threshold did not filter lower levels");

    const std::string output = capture.output();
    check(output.find("filtered-") == std::string::npos, "filtered message was printed");
    check(output.find("allowed-warning 1") != std::string::npos, "WARN message missing");
    check(output.find("allowed-error 2") != std::string::npos, "ERROR message missing");
}

void test_log_if_else() {
    StderrCapture capture;
    netlib::Logger::setLevel(netlib::LogLevel::WARN);
    bool outerElse = false;
    if (true)
        NETLIB_LOG_INFO << "filtered";
    else
        outerElse = true;
    check(!outerElse, "else bound to the logging macro");

    if (false)
        NETLIB_LOG_WARN << "unreachable";
    else
        outerElse = true;
    check(outerElse, "outer else did not execute");

    if (true)
        NETLIB_LOG_ERROR << "reachable";
    else
        outerElse = false;
    check(outerElse, "enabled log changed outer control flow");
    check(capture.output().find("reachable") != std::string::npos, "enabled log missing");
}

void test_log_threshold_snapshot() {
    StderrCapture capture;
    NETLIB_LOG_INFO << ([]() {
        netlib::Logger::setLevel(netlib::LogLevel::FATAL);
        return "accepted-record";
    })();
    NETLIB_LOG_INFO << "subsequent-record";
    const std::string output = capture.output();
    check(output.find("accepted-record") != std::string::npos,
          "accepted log was filtered again during destruction");
    check(output.find("subsequent-record") == std::string::npos,
          "new threshold did not apply to the next log");
}

void test_log_errno() {
    StderrCapture capture;
    errno = ENOENT;
    const int savedErrno = errno;
    NETLIB_LOG_ERROR << "saved errno=" << savedErrno << ([]() {
        errno = EIO;
        return ", formatting changed errno";
    })();
    check(errno == savedErrno, "logging did not restore the caller's errno");
    check(capture.output().find("saved errno=" + std::to_string(savedErrno))
          != std::string::npos, "original error code missing");
}

void test_log_assert() {
    int evaluated = 0;
    NETLIB_ASSERT(++evaluated == 1);
    check(evaluated == 1, "passing assertion did not evaluate exactly once");
    bool outerElse = false;
    if (false)
        NETLIB_ASSERT(false);
    else
        outerElse = true;
    check(outerElse, "assertion macro changed outer control flow");
}

// 在子进程中验证 abort, 防止终止测试主进程或留下 core 文件
template <typename Callback>
static std::string expectAbort(Callback callback) {
    StderrCapture capture;
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const rlimit limit{0, 0};
        if (::setrlimit(RLIMIT_CORE, &limit) != 0)
            ::_exit(2);
        callback();
        ::_exit(0);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child, "waitpid failed");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "child did not terminate with SIGABRT");
    return capture.output();
}

void test_log_fatal() {
    const std::string output = expectAbort([]() {
        netlib::Logger::setLevel(netlib::LogLevel::FATAL);
        NETLIB_LOG_FATAL << "fatal-before-abort";
    });
    check(output.find("FATAL") != std::string::npos, "FATAL level missing");
    check(output.find("fatal-before-abort") != std::string::npos,
          "FATAL message was not output before abort");
}

void test_log_assert_failure() {
    const std::string output = expectAbort([]() {
        auto condition = []() {
            std::fputs("condition-evaluated\n", stderr);
            return false;
        };
        NETLIB_ASSERT(condition());
    });
    check(output.find("assertion failed: condition()") != std::string::npos,
          "failed assertion did not report its expression");
    const size_t first = output.find("condition-evaluated");
    check(first != std::string::npos, "failed assertion condition was not evaluated");
    check(output.find("condition-evaluated", first + 1) == std::string::npos,
          "failed assertion condition was evaluated more than once");
}

void test_log_concurrent() {
    StderrCapture capture;
    constexpr int workers = 4;
    constexpr int messages = 32;
    std::atomic<bool> done{false};
    std::thread setter([&]() {
        while (!done.load(std::memory_order_relaxed)) {
            netlib::Logger::setLevel(netlib::LogLevel::DEBUG);
            netlib::Logger::setLevel(netlib::LogLevel::INFO);
            std::this_thread::yield();
        }
    });

    std::vector<std::string> threadIds(workers);
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            std::ostringstream threadId;
            threadId << std::this_thread::get_id();
            threadIds[worker] = threadId.str();
            for (int entry = 0; entry < messages; ++entry)
                NETLIB_LOG_WARN << "<worker=" << worker << ";entry=" << entry << ">";
        });
    }
    for (auto& thread : threads)
        thread.join();
    done.store(true, std::memory_order_relaxed);
    setter.join();

    const std::string output = capture.output();
    for (int worker = 0; worker < workers; ++worker) {
        for (int entry = 0; entry < messages; ++entry) {
            const std::string marker = "<worker=" + std::to_string(worker)
                                     + ";entry=" + std::to_string(entry) + ">";
            const size_t position = output.find(marker);
            check(position != std::string::npos, "concurrent message lost or interleaved");
            check(output.find(marker, position + 1) == std::string::npos,
                  "concurrent message duplicated");
            const size_t newline = output.rfind('\n', position);
            const size_t start = newline == std::string::npos ? 0 : newline + 1;
            const std::string prefix = output.substr(start, position - start);
            check(prefix.find("[tid=" + threadIds[worker] + "]") != std::string::npos,
                  "concurrent message attributed to the wrong thread");
        }
    }
}

int main() {
    struct TestCase {
        const char* name;
        void (*run)();
    };
    const TestCase tests[] = {
        {"basic", test_log_basic},
        {"level_filter", test_log_level_filter},
        {"if_else", test_log_if_else},
        {"threshold_snapshot", test_log_threshold_snapshot},
        {"errno", test_log_errno},
        {"assert", test_log_assert},
        {"fatal", test_log_fatal},
        {"assert_failure", test_log_assert_failure},
        {"concurrent", test_log_concurrent},
    };
    for (const auto& test : tests) {
        netlib::Logger::setLevel(netlib::LogLevel::DEBUG);
        test.run();
        std::cout << test.name << " passed" << std::endl;
    }
    netlib::Logger::setLevel(netlib::LogLevel::DEBUG);
    std::cout << "test_log: all passed" << std::endl;
    return 0;
}
