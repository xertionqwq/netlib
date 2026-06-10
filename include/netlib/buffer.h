#ifndef NETLIB_BUFFER_H
#define NETLIB_BUFFER_H

#include <sys/uio.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace netlib {

class Buffer {
    /*
    ** netBuffer — read/write pointor + vector<char>
    ** [prependable | readable | writable]
    **  0    kCheapPrepend  readerIdx_   writerIdx_   capacity
    ** copyable
    */
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize  = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buf_(kCheapPrepend + initialSize),
          readerIdx_(kCheapPrepend),
          writerIdx_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIdx_ - readerIdx_; }
    const char* peek() const { return begin() + readerIdx_; }

    void retrieve(size_t len) {
        assert(len <= readableBytes());
        if (len < readableBytes())
            readerIdx_ += len;
        else
            retrieveAll();
    }

    void retrieveAll() {
        readerIdx_ = kCheapPrepend;
        writerIdx_ = kCheapPrepend;
    }

    std::string retrieveAsString(size_t len) {
        assert(len <= readableBytes());
        std::string s(peek(), len);
        retrieve(len);
        return s;
    }

    size_t writableBytes() const { return buf_.size() - writerIdx_; }
    void hasWritten(size_t len) { writerIdx_ += len; }

    ssize_t readFd(int fd) {
        char extrabuf[65536];
        struct iovec vec[2];
        vec[0].iov_base = begin() + writerIdx_;
        vec[0].iov_len  = writableBytes();
        vec[1].iov_base = extrabuf;
        vec[1].iov_len  = sizeof extrabuf;

        ssize_t n = ::readv(fd, vec, 2);
        if (n < 0) return n;

        if (static_cast<size_t>(n) <= writableBytes()) {
            writerIdx_ += n;
        } else {
            writerIdx_ = buf_.size();
            append(extrabuf, n - vec[0].iov_len);
        }
        return n;
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        ::memcpy(begin() + writerIdx_, data, len);
        writerIdx_ += len;
    }

    void append(const std::string& str) { append(str.data(), str.size()); }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len)
            makeSpace(len);
        assert(writableBytes() >= len);
    }

private:
    char* begin() { return buf_.data(); }
    const char* begin() const { return buf_.data(); }

    void makeSpace(size_t len) {
        size_t prepend  = readerIdx_;
        size_t readable  = readableBytes();

        if (prepend + writableBytes() < len + kCheapPrepend) {
            buf_.resize(writerIdx_ + len);
        } else {
            ::memmove(begin() + kCheapPrepend, peek(), readable);
            readerIdx_ = kCheapPrepend;
            writerIdx_ = kCheapPrepend + readable;
        }
    }

    std::vector<char> buf_;
    size_t readerIdx_;
    size_t writerIdx_;
};

}  // namespace netlib

#endif  // NETLIB_BUFFER_H
