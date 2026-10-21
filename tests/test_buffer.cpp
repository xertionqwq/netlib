#include "netlib/buffer.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netlib/noncopyable.h"

using netlib::Buffer;

Buffer makeBufferInOtherTranslationUnit(size_t initialSize);
void appendInOtherTranslationUnit(Buffer& buffer, const std::string& data);
const size_t* bufferConstantInOtherTranslationUnit();

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_buffer failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

static void checkContent(const Buffer& buffer, const std::string& expected) {
    check(buffer.readableBytes() == expected.size(), "incorrect readable length");
    check(std::string(buffer.peek(), buffer.readableBytes()) == expected,
          "buffer contents changed or arrived out of order");
}

static std::string pattern(size_t size) {
    std::string data(size, '\0');
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<char>('!' + i % 83);
    return data;
}

class Pipe : private netlib::Noncopyable {
public:
    Pipe() {
        check(::pipe2(fds_, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2 failed");
    }
    ~Pipe() {
        ::close(fds_[0]);
        if (fds_[1] >= 0)
            ::close(fds_[1]);
    }
    int reader() const { return fds_[0]; }
    int writer() const { return fds_[1]; }
    void closeWriter() {
        check(::close(fds_[1]) == 0, "closing writer failed");
        fds_[1] = -1;
    }
private:
    int fds_[2];
};

static void writeBytes(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        check(n > 0, "writing pipe data failed");
        offset += static_cast<size_t>(n);
    }
}

template <typename Callback>
static void expectAbort(Callback callback) {
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const rlimit limit{0, 0};
        if (::setrlimit(RLIMIT_CORE, &limit) != 0 ||
            std::freopen("/dev/null", "w", stderr) == nullptr)
            ::_exit(2);
        callback();
        ::_exit(0);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child, "waitpid failed");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "invalid input did not trigger a persistent assertion");
}

static void test_initial_sizes_and_strings() {
    for (size_t size : {size_t{0}, size_t{1}, size_t{16}, Buffer::kInitialSize, size_t{4096}}) {
        Buffer buffer(size);
        check(buffer.readableBytes() == 0 && buffer.writableBytes() == size &&
              buffer.prependableBytes() == Buffer::kCheapPrepend,
              "custom initial size was not respected");
        check(buffer.retrieveAllAsString().empty(), "empty retrieval returned data");
        buffer.append(nullptr, 0);
        const std::string binary("a\0bc", 4);
        buffer.append(binary);
        check(buffer.retrieveAsString(2) == binary.substr(0, 2), "partial binary retrieval failed");
        checkContent(buffer, binary.substr(2));
        check(buffer.retrieveAllAsString() == binary.substr(2), "full retrieval failed");
        check(buffer.readableBytes() == 0 &&
              buffer.prependableBytes() == Buffer::kCheapPrepend,
              "full retrieval did not reset the read position");
    }
}

static void test_compaction_and_growth() {
    Buffer buffer(16);
    const std::string data = pattern(16);
    buffer.append(data);
    const char* initial = buffer.peek();
    buffer.retrieve(8);
    buffer.append("tail", 4);
    check(buffer.peek() == initial, "consumed space was not reclaimed in place");
    checkContent(buffer, data.substr(8) + "tail");

    const std::string extra = pattern(4096);
    buffer.append(extra);
    checkContent(buffer, data.substr(8) + "tail" + extra);
    buffer.retrieveAll();
    buffer.append("reuse", 5);
    checkContent(buffer, "reuse");
}

static void test_self_append() {
    Buffer spare(64);
    spare.append("abcdef", 6);
    spare.append(spare.peek() + 1, 3);
    checkContent(spare, "abcdefbcd");

    Buffer compact(16);
    const std::string data = pattern(16);
    compact.append(data);
    compact.retrieve(4);
    compact.append(compact.peek() + 1, 3);
    checkContent(compact, data.substr(4) + data.substr(5, 3));

    Buffer grow(8);
    grow.append("abcdefgh", 8);
    grow.append(grow.peek() + 1, 6);
    checkContent(grow, "abcdefghbcdefg");
}

static void test_read_fd() {
    for (size_t size : {size_t{0}, size_t{16}, size_t{65536 + 8}}) {
        Pipe pipe;
        Buffer buffer(size);
        buffer.append("old", 3);
        const std::string data = pattern(2048);
        writeBytes(pipe.writer(), data);
        int savedErrno = 123;
        const ssize_t n = buffer.readFd(pipe.reader(), &savedErrno);
        check(n == static_cast<ssize_t>(data.size()), "readv returned an incorrect byte count");
        check(savedErrno == 123, "successful read modified the error output");
        checkContent(buffer, "old" + data);

        pipe.closeWriter();
        check(buffer.readFd(pipe.reader(), &savedErrno) == 0, "closed writer did not produce EOF");
        checkContent(buffer, "old" + data);
    }
}

static void test_read_errors_and_preconditions() {
    Pipe pipe;
    Buffer buffer;
    buffer.append("keep", 4);
    int savedErrno = 0;
    check(buffer.readFd(pipe.reader(), &savedErrno) == -1 &&
          (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK),
          "empty nonblocking pipe did not report EAGAIN");
    checkContent(buffer, "keep");
    check(buffer.readFd(-1, &savedErrno) == -1 && savedErrno == EBADF,
          "invalid fd did not preserve EBADF");
    checkContent(buffer, "keep");

    writeBytes(pipe.writer(), "x");
    expectAbort([&] { buffer.readFd(pipe.reader(), nullptr); });
    char byte = '\0';
    check(::read(pipe.reader(), &byte, 1) == 1 && byte == 'x',
          "readFd consumed input before validating its error output");
}

static void test_persistent_bounds_checks() {
    expectAbort([] { Buffer buffer; buffer.retrieve(1); });
    expectAbort([] { Buffer buffer; buffer.retrieveAsString(1); });
    expectAbort([] { Buffer buffer(0); buffer.hasWritten(1); });
    expectAbort([] { Buffer buffer; buffer.append(nullptr, 1); });
    expectAbort([] {
        Buffer buffer;
        buffer.append("abc", 3);
        buffer.append(buffer.peek() + 1, 3);
    });
    expectAbort([] {
        Buffer buffer;
        buffer.append(buffer.peek() - 1, 1);
    });
}

static void test_oversized_requests() {
    bool rejected = false;
    try {
        Buffer buffer(std::numeric_limits<size_t>::max());
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected, "initial size overflow was not rejected");

    Buffer buffer;
    buffer.append("keep", 4);
    rejected = false;
    try {
        buffer.ensureWritableBytes(std::numeric_limits<size_t>::max());
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected, "growth size overflow was not rejected");
    checkContent(buffer, "keep");
}

static void test_copy_and_multiple_translation_units() {
    auto buffer = makeBufferInOtherTranslationUnit(32);
    appendInOtherTranslationUnit(buffer, "original");
    Buffer copy = buffer;
    copy.retrieve(3);
    copy.append("!", 1);
    checkContent(buffer, "original");
    checkContent(copy, "ginal!");
    check(bufferConstantInOtherTranslationUnit() == &Buffer::kCheapPrepend,
          "constexpr constant has different addresses across translation units");
}

int main() {
    test_initial_sizes_and_strings();
    test_compaction_and_growth();
    test_self_append();
    test_read_fd();
    test_read_errors_and_preconditions();
    test_persistent_bounds_checks();
    test_oversized_requests();
    test_copy_and_multiple_translation_units();
    std::cout << "test_buffer: all passed\n";
    return 0;
}
