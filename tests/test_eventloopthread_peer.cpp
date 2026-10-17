#include "netlib/eventloopthread.h"

netlib::EventLoop* startLoopInOtherTranslationUnit(netlib::EventLoopThread& thread) {
    return thread.startLoop();
}

netlib::EventLoop* currentLoopInOtherTranslationUnit() {
    return netlib::loopPtr;
}
