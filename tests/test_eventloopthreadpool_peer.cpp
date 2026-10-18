#include "netlib/eventloopthreadpool.h"

void startPoolInOtherTranslationUnit(netlib::EventLoopThreadPool& pool) {
    pool.start();
}

netlib::EventLoop* nextPoolLoopInOtherTranslationUnit(netlib::EventLoopThreadPool& pool) {
    return pool.getNextLoop();
}

netlib::EventLoop* currentPoolLoopInOtherTranslationUnit() {
    return netlib::loopPtr;
}
