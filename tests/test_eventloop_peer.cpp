#include "netlib/eventloop.h"

void constructEventLoopInOtherTranslationUnit() {
    netlib::EventLoop loop;
}

pid_t threadIdInOtherTranslationUnit() {
    return netlib::CurrentThread::tid();
}
