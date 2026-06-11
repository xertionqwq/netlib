#include <thread>
#include <iostream>

#include "netlib/log.h"
#include "netlib/thread.h"

using netlib::Semaphore;

// ============================================================
// 测试用例
// ============================================================

// 1. 基本单线程 wait/signal (count=1)
void test_signal_then_wait() {
    Semaphore sem(1);
    sem.wait();  // count 1→0，立即返回
    // 不阻塞 = 通过
}

// 2. count=0 时 wait 会阻塞，signal 后恢复
void test_wait_blocks_then_signal() {
    Semaphore sem(0);
    bool done = false;
    std::thread t([&]() {
        sem.wait();   // count=0，阻塞
        done = true;  // signal 后才执行到这里
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // t 应该在阻塞中，done 还为 false
    if (done) {
        NETLIB_LOG_ERROR << "should still be blocked";
        t.join();
        return;
    }

    sem.signal();  // count 0→1，唤醒 t
    t.join();
    assert(done && "should be done after signal");
}

// 3. 计数信号量：count=N 允许 N 个线程同时通过
void test_counting_semaphore() {
    const int N = 3;
    Semaphore sem(N);
    int concurrent = 0, maxConcurrent = 0;
    std::mutex mtx;

    auto worker = [&]() {
        sem.wait();  // count>0，进入
        {
            std::lock_guard<std::mutex> lock(mtx);
            ++concurrent;
            if (concurrent > maxConcurrent) maxConcurrent = concurrent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard<std::mutex> lock(mtx);
            --concurrent;
        }
        sem.signal();  // 释放
    };

    std::thread threads[N];
    for (int i = 0; i < N; ++i) threads[i] = std::thread(worker);
    for (int i = 0; i < N; ++i) threads[i].join();

    assert(maxConcurrent == N && "all N should run concurrently");
}

// 4. 生产者-消费者：多 signal 积累计数
void test_multi_signal() {
    Semaphore sem(0);

    // 消费者：等 3 次 signal
    std::thread consumer([&]() {
        for (int i = 0; i < 3; ++i) sem.wait();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sem.signal();
    sem.signal();
    sem.signal();

    consumer.join();  // 不阻塞 = 通过
}

// 5. 两个线程轮流执行 (ping-pong)
void test_ping_pong() {
    Semaphore semA(1), semB(0);
    int step = 0;

    std::thread t1([&]() {
        semA.wait();
        assert(step == 0); step = 1;  // A 先
        semB.signal();

        semA.wait();
        assert(step == 2); step = 3;  // A 再
        semB.signal();
    });

    std::thread t2([&]() {
        semB.wait();
        assert(step == 1); step = 2;  // B 后
        semA.signal();

        semB.wait();
        assert(step == 3); step = 4;  // B 后
        semA.signal();
    });

    t1.join();
    t2.join();
    assert(step == 4);
}

int main() {
    test_signal_then_wait();
    std::cout << "[1/5] test_signal_then_wait passed" << std::endl;

    test_wait_blocks_then_signal();
    std::cout << "[2/5] test_wait_blocks_then_signal passed" << std::endl;

    test_counting_semaphore();
    std::cout << "[3/5] test_counting_semaphore passed" << std::endl;

    test_multi_signal();
    std::cout << "[4/5] test_multi_signal passed" << std::endl;

    test_ping_pong();
    std::cout << "[5/5] test_ping_pong passed" << std::endl;

    std::cout << "test_semaphore: all passed" << std::endl;
    return 0;
}
