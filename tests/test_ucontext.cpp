#include <ucontext.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
using namespace std;

// ============================================================
// ucontext 测试 — 熟悉 getcontext / makecontext / swapcontext / setcontext
// ============================================================

// TODO:
void fun(int arg) {
    cout << "1" << endl;
    cout << "11" << endl;
    cout << "111" << endl;
    cout << "1111" << endl;
} // 函数交给 makecontext 使用

void context_test() {
    char stack[1024 * 128]; // 创建栈空间
    ucontext_t child, main; // 设置上下文
    getcontext(&child); // 将上下文信息保存至 child 中

    child.uc_stack.ss_sp = stack; // 制定栈空间
    child.uc_stack.ss_size = sizeof(stack); // 制定栈空间大小
    child.uc_stack.ss_flags = 0;
    child.uc_link = &main; // 设置后继上下文

    makecontext(&child, (void(*)(void))fun, 0); // 将 child 的上下文函数执行设为 fun
    swapcontext(&main, &child); // 将当前上下文设置到 main 中, 同时执行 child 上下文
    cout << "main" << endl;
}

int main() {
    context_test();
    std::cout << "test_ucontext: all passed" << std::endl;
    return 0;
}
