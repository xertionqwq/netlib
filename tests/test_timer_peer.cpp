#include "netlib/timer.h"

int64_t timerSequenceInOtherTranslationUnit() {
    netlib::Timer timer([] {}, netlib::Timestamp(1'000'000));
    return timer.sequence();
}

netlib::Timestamp addTimeInOtherTranslationUnit(netlib::Timestamp now, double seconds) {
    return netlib::addTime(now, seconds);
}
