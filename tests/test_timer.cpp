#include "netlib/timestamp.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "netlib/timer.h"

int64_t timerSequenceInOtherTranslationUnit();
netlib::Timestamp addTimeInOtherTranslationUnit(netlib::Timestamp now, double seconds);

namespace {

// 关键检查在 NDEBUG 下同样执行。
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_timer failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkDifference(netlib::Timestamp high, netlib::Timestamp low, double seconds) {
    check(std::abs(netlib::timeDifference(high, low) - seconds) < 1e-12,
          "time difference has incorrect units or lost fractional seconds");
}

void testTimestampUnits() {
    using netlib::Timestamp;
    const Timestamp epoch(1'700'000'000'123'456LL);
    check(epoch.microSecondsSinceEpoch() == 1'700'000'000'123'456LL,
          "Timestamp constructor must accept microseconds");
    check(epoch.secondsSinceEpoch() == 1'700'000'000,
          "seconds accessor must remove the fractional part");
    check(epoch.toString() == "1700000000.123456", "incorrect timestamp text");
    check(Timestamp(5'000'000'000'000'007LL).toString() == "5000000000.000007",
          "timestamp formatting truncated 64-bit seconds or lost zero padding");
    check(Timestamp::invalid() == Timestamp(), "inconsistent invalid timestamp");
    check(Timestamp::invalid().toString() == "0.000000", "incorrect zero timestamp text");

    checkDifference(Timestamp(1'500'000), Timestamp(1'000'000), 0.5);
    checkDifference(Timestamp(1'000'000), Timestamp(1'500'000), -0.5);
    checkDifference(Timestamp(1'100'000), Timestamp(900'000), 0.2);
    checkDifference(Timestamp(1'000'001), Timestamp(1'000'000), 0.000001);
    checkDifference(Timestamp(2'000'000), Timestamp(1'000'000), 1.0);
    checkDifference(epoch, epoch, 0.0);

    check(netlib::addTime(epoch, 0.5).microSecondsSinceEpoch() == 1'700'000'000'623'456LL,
          "adding a fractional second lost precision");
    check(netlib::addTime(epoch, -0.125).microSecondsSinceEpoch() == 1'699'999'999'998'456LL,
          "subtracting a fractional second did not cross the second boundary");
    check(netlib::addTime(epoch, 0.000001).microSecondsSinceEpoch()
              == 1'700'000'000'123'457LL,
          "adding one microsecond lost precision");
    check(netlib::addTime(epoch, 0.0000005) == epoch,
          "sub-microsecond interval must truncate to the stored resolution");
    check(addTimeInOtherTranslationUnit(epoch, 0.5) == netlib::addTime(epoch, 0.5),
          "time calculation differs between translation units");

    Timestamp earlier(1'000'000);
    Timestamp later(1'000'001);
    check(earlier < later, "timestamp comparison lost microseconds");
    earlier.swap(later);
    check(earlier.microSecondsSinceEpoch() == 1'000'001
              && later.microSecondsSinceEpoch() == 1'000'000,
          "timestamp swap lost microseconds");
}

void testTimestampNow() {
    using namespace std::chrono;
    const auto before = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    const auto now = netlib::Timestamp::now().microSecondsSinceEpoch();
    const auto after = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    check(now >= before && now <= after, "Timestamp::now must return Unix time in microseconds");
}

void testTimerRestart() {
    using netlib::Timestamp;
    int calls = 0;
    const Timestamp firstExpiration(10'250'000);
    netlib::Timer timer([&] { ++calls; }, firstExpiration, 0.125);
    check(timer.repeat(), "positive sub-second interval must create a repeating timer");
    check(timer.expiration() == firstExpiration, "interval changed the first expiration");
    check(calls == 0, "Timer constructor must not execute the callback");
    const int64_t sequence = timer.sequence();
    timer.run();
    check(calls == 1, "Timer::run must execute the callback once");
    timer.restart(Timestamp(20'500'000));
    check(timer.expiration() == Timestamp(20'625'000),
          "repeating timer must restart from now plus the fractional interval");
    check(timer.sequence() == sequence, "restarting changed the timer identity");
    check(calls == 1, "Timer::restart must not execute the callback");

    netlib::Timer oneShot([&] { ++calls; }, firstExpiration);
    check(!oneShot.repeat(), "default timer must run once");
    oneShot.run();
    oneShot.restart(Timestamp(20'500'000));
    check(calls == 2, "one-shot callback was not executed exactly once");
    check(oneShot.expiration() == Timestamp::invalid(),
          "one-shot restart must invalidate the expiration");
}

void testTimerSequenceAcrossTranslationUnits() {
    const int64_t before = netlib::Timer::numCreated();
    netlib::Timer timer([] {}, netlib::Timestamp(1'000'000));
    check(timer.sequence() == before + 1, "timer sequence did not advance");
    check(timerSequenceInOtherTranslationUnit() == before + 2,
          "timer sequence counter is not shared between translation units");
    check(netlib::Timer::numCreated() == before + 2,
          "timer creation count must be cumulative across translation units");
}

} // namespace

int main() {
    testTimestampUnits();
    testTimestampNow();
    testTimerRestart();
    testTimerSequenceAcrossTranslationUnits();
    std::cout << "test_timer: all passed\n";
}
