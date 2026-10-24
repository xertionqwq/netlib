#include "netlib/eventloop.h"

void constructEventLoopInOtherTranslationUnit() {
    netlib::EventLoop loop;
}

pid_t threadIdInOtherTranslationUnit() {
    return netlib::CurrentThread::tid();
}

netlib::EventLoop* checkLoopInOtherTranslationUnit(netlib::EventLoop* loop) {
    return netlib::checkLoop(loop);
}

netlib::EventLoop* checkOwnerLoopInOtherTranslationUnit(netlib::EventLoop* loop) {
    return netlib::checkLoopInThread(loop);
}
