#ifndef NETLIB_NONCOPYABLE_H
#define NETLIB_NONCOPYABLE_H

namespace netlib {

/*
** 不可拷贝对象, 继承后子类可以正常构造和析构
** 无法完成父类的拷贝构造和拷贝赋值
*/

class Noncopyable {
public:
    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;

protected:
    Noncopyable() = default;
    ~Noncopyable() = default;
};

} // namespace netlib

#endif // NETLIB_NONCOPYABLE_H
