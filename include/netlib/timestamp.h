#ifndef NETLIB_TIMESTAMP_H
#define NETLIB_TIMESTAMP_H

#include <string>
#include <ctime>

namespace netlib{

// 时间类
class Timestamp {
public:
    Timestamp() = default;
    explicit Timestamp(std::time_t seconds)
    : secondsSinceEpoch_(seconds) {}

    static Timestamp now(); // 静态函数便于调用
    std::string toString() const;

private:
    std::time_t secondsSinceEpoch_ = 0;
};

inline Timestamp Timestamp::now() {
    return Timestamp(std::time(nullptr));
}

inline std::string Timestamp::toString() const {
    std::tm localTime{};
    if (::localtime_r(&secondsSinceEpoch_, &localTime) == nullptr)
        return {};
    char timeBuf[32]{};
    const auto length = std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localTime);
    return std::string(timeBuf, length);
}

} // netlib

#endif // NETLIB_TIMESTAMP_H
