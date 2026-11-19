#ifndef NETLIB_TIMESTAMP_H
#define NETLIB_TIMESTAMP_H

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <sys/time.h>

namespace netlib{

// 以微秒存储 Unix 时间戳
// 推荐按值传递, 因为在 x64 寄存器内传递
class Timestamp {
public:
    Timestamp() = default; // 非法时间类
    explicit Timestamp(int64_t microSeconds)
    : microSecondsSinceEpoch_(microSeconds) {}

    void swap(Timestamp &other) {
        std::swap(microSecondsSinceEpoch_, other.microSecondsSinceEpoch_);
    }

    static Timestamp now(); // 静态函数便于调用
    static Timestamp invalid();
    std::string toString() const;

    bool valid() const { return microSecondsSinceEpoch_ > 0; }

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
    std::time_t secondsSinceEpoch() const {
        return static_cast<std::time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
    }

    bool operator<(Timestamp other) const {
        return microSecondsSinceEpoch_ < other.microSecondsSinceEpoch_;
    }
    bool operator==(Timestamp other) const {
        return microSecondsSinceEpoch_ == other.microSecondsSinceEpoch_;
    }
public:
    // 微秒-秒转换单位
    static constexpr int kMicroSecondsPerSecond = 1000 * 1000;

private:
    int64_t microSecondsSinceEpoch_ = 0; // 整数, 单位为微秒
};

static_assert(sizeof(Timestamp) == sizeof(int64_t),
              "Timestamp should be same size as int64_t");

inline Timestamp Timestamp::now() {
    struct timeval tv{};
    ::gettimeofday(&tv, nullptr);
    const int64_t seconds = tv.tv_sec;
    return Timestamp(seconds * kMicroSecondsPerSecond + tv.tv_usec);
}

inline Timestamp Timestamp::invalid() {
    return Timestamp();
}

inline std::string Timestamp::toString() const {
    char buf[32] = {0};
    const int64_t seconds = microSecondsSinceEpoch_ / kMicroSecondsPerSecond;
    const int64_t microseconds = microSecondsSinceEpoch_ % kMicroSecondsPerSecond;
    std::snprintf(buf, sizeof(buf), "%" PRId64 ".%06" PRId64, seconds, microseconds);
    return buf;
}

// 非 Timestamp 静态函数
// 返回 high-low 的时间差, 单位为秒, 保留微秒部分
inline double timeDifference(Timestamp high, Timestamp low) {
    const int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;
}

// 非 Timestamp 类静态函数
// 加上 seconds 秒并返回 Timestamp 类, 不足一微秒的部分截断
inline Timestamp addTime(Timestamp curTime, double seconds) {
    const int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
    return Timestamp(curTime.microSecondsSinceEpoch() + delta);
}

} // netlib

#endif // NETLIB_TIMESTAMP_H
