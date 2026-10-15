#ifndef NETLIB_CURRENTTHREAD_H
#define NETLIB_CURRENTTHREAD_H

#include <unistd.h>
#include <sys/syscall.h>

namespace netlib {

namespace CurrentThread {

    inline thread_local pid_t t_cachedTid = 0;

    inline void cacheTid() {
        if (t_cachedTid == 0) // 获取当前线程的 Tid 值
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
    }

    inline pid_t tid() {
        if (__builtin_expect(t_cachedTid == 0, 0))
            cacheTid(); // 没有获取过 tid, 则获取
        return t_cachedTid;
    }
} // CurrentThread

} // netlib

#endif // NETLIB_CURRENTTHREAD_H
