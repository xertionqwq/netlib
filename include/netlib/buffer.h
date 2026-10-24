#ifndef NETLIB_BUFFER_H
#define NETLIB_BUFFER_H

#include <sys/uio.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "netlib/log.h" // 采用 NDEBUG 下也有效的断言

namespace netlib {

// 网络库底层缓冲区
class Buffer {
    /*
    ** netBuffer — read/write pointor + vector<char>
    ** +------------------------+--------------------+-------------------+
    ** [   prependable bytes    |   readable bytes   |   writable bytes  ]
    ** |                        |     (CONTENT)      |                   |
    ** +------------------------+--------------------+-------------------+
    ** 0    kCheapPrepend   readerIdx_           writerIdx_           buf_.size()
    ** copyable
    */
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize  = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buf_(initialBufferSize(initialSize)),
          readerIdx_(kCheapPrepend),
          writerIdx_(kCheapPrepend) {
        NETLIB_ASSERT(readableBytes() == 0);
        NETLIB_ASSERT(writableBytes() == initialSize);
        NETLIB_ASSERT(prependableBytes() == kCheapPrepend);
    }

    // 可读多少
    size_t readableBytes() const { return writerIdx_ - readerIdx_; }
    // 可写多少
    size_t writableBytes() const { return buf_.size() - writerIdx_; }
    // 已空多少
    size_t prependableBytes() const { return readerIdx_; }
    // 返回缓存区中可读的起始地址
    const char* peek() const { return begin() + readerIdx_; }

    void retrieve(size_t len) {
        NETLIB_ASSERT(len <= readableBytes());
        if (len < readableBytes())
            readerIdx_ += len;
        else
            retrieveAll();
    }

    void retrieveAll() { // 读完了, 指针复位用于重应用
        readerIdx_ = kCheapPrepend;
        writerIdx_ = kCheapPrepend;
    }

    // 将 onMessage 函数上报的 Buffer 数据, 转成 string 类型返回
    std::string retrieveAsString(size_t len) {
        NETLIB_ASSERT(len <= readableBytes());
        std::string s(peek(), len); // 构造可读子串
        retrieve(len); // 消费移动指针
        return s;
    }
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes()); // 可读取的长度
    }

    void hasWritten(size_t len) {
        NETLIB_ASSERT(len <= writableBytes());
        writerIdx_ += len;
    }

    // 从 fd 上读取数据；savedErrno 必须非空，仅在读取失败时写入错误码。
    ssize_t readFd(int fd, int *savedErrno) {
        NETLIB_ASSERT(savedErrno != nullptr);
        char extrabuf[65536]; // 栈上内存空间 64k，只使用实际读到的字节
        const size_t writable = writableBytes();
        struct iovec vec[2]; // 拿出两块缓冲区
        // 缓冲区一, 也就是当前的 buf_
        vec[0].iov_base = beginWrite();
        vec[0].iov_len = writable;
        // 缓存区二, 也就是栈上的 extrabuf
        vec[1].iov_base = extrabuf;
        vec[1].iov_len  = sizeof extrabuf;

        const int iovcnt = writable < sizeof(extrabuf) ? 2 : 1; // 根据 buf_ 缓冲区大小选择缓冲区个数
        ssize_t n = ::readv(fd, vec, iovcnt); // readv 可以将数据读到多个非连续的缓冲区
        if (n < 0) {
            *savedErrno = errno;
            return n;
        }

        if (static_cast<size_t>(n) <= writable) { // 只往 buf_ 里面写了
            hasWritten(static_cast<size_t>(n));
        } else { // buf_ 写满了, 再写 extrabuf
            writerIdx_ = buf_.size();
            append(extrabuf, static_cast<size_t>(n) - writable); // 将 extrabuf 的数据追加至 buf_
        }
        return n;
    }
    // 将可读数据写入 fd；savedErrno 必须非空，仅在失败时写入错误码。
    // 成功时只消费实际写出的字节；重试与 SIGPIPE 策略由调用方管理。
    ssize_t writeFd(int fd, int *savedErrno) {
        NETLIB_ASSERT(savedErrno != nullptr);
        const ssize_t n = ::write(fd, peek(), readableBytes());
        if (n < 0) {
            *savedErrno = errno;
            return n;
        }
        retrieve(static_cast<size_t>(n)); // 只消费成功写出的数据
        return n;
    }

    // 将 [data, data + len) 追加到缓冲区；也支持来自当前可读区的自追加。
    // len 为 0 时允许 data 为空。搬移或扩容后，原有的缓冲区指针可能失效。
    void append(const char* data, size_t len) {
        if (len == 0)
            return;
        NETLIB_ASSERT(data != nullptr);

        std::string snapshot;
        const std::less<const char*> less;
        if (!less(data, begin()) && less(data, begin() + buf_.size())) {
            NETLIB_ASSERT(!less(data, peek()) && less(data, beginWrite()));
            NETLIB_ASSERT(len <= static_cast<size_t>(beginWrite() - data));
            if (writableBytes() < len) {
                // makeSpace 可能搬移数据或使 vector 重新分配，先保留原内容。
                snapshot.assign(data, len);
                data = snapshot.data();
            }
        }
        ensureWritableBytes(len);
        ::memmove(beginWrite(), data, len);
        hasWritten(len);
    }

    void append(const std::string& str) { append(str.data(), str.size()); }

    // 保证缓存区还能写入 len, 否则进行扩容
    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len)
            makeSpace(len);
        NETLIB_ASSERT(writableBytes() >= len);
    }

private:
    static size_t initialBufferSize(size_t initialSize) {
        if (initialSize > std::numeric_limits<size_t>::max() - kCheapPrepend)
            throw std::length_error("Buffer initial size is too large");
        return kCheapPrepend + initialSize;
    }

    char *begin() { return buf_.data(); }
    const char *begin() const { return buf_.data(); }
    char *beginWrite() { return begin() + writerIdx_; }
    const char *beginWrite() const { return begin() + writerIdx_; }

    void makeSpace(size_t len) {
        const size_t reclaimable = prependableBytes() - kCheapPrepend;
        const size_t readable = readableBytes();
        NETLIB_ASSERT(len > writableBytes());

        if (len - writableBytes() > reclaimable) {
            // 空闲 + 可写仍不满足
            if (len > buf_.max_size() - writerIdx_)
                throw std::length_error("Buffer requested size is too large");
            buf_.resize(writerIdx_ + len);
        } else {
            // 将可读数据搬到预留区之后，回收前面已经消费的空间。
            // kCheapPrepend <-- readerIdx_
            ::memmove(begin() + kCheapPrepend, peek(), readable);
            // 调整水位
            readerIdx_ = kCheapPrepend;
            writerIdx_ = kCheapPrepend + readable;
        }
    }

    std::vector<char> buf_; // 当前维护的缓冲区
    size_t readerIdx_;      // 可读部分的起始
    size_t writerIdx_;      // 可写部分的起始
};

}  // namespace netlib

#endif  // NETLIB_BUFFER_H
