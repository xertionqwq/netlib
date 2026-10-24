#include "netlib/buffer.h"

netlib::Buffer makeBufferInOtherTranslationUnit(size_t initialSize) {
    return netlib::Buffer(initialSize);
}

void appendInOtherTranslationUnit(netlib::Buffer& buffer, const std::string& data) {
    buffer.append(data);
}

const size_t* bufferConstantInOtherTranslationUnit() {
    return &netlib::Buffer::kCheapPrepend;
}

ssize_t writeInOtherTranslationUnit(netlib::Buffer& buffer, int fd, int* savedErrno) {
    return buffer.writeFd(fd, savedErrno);
}
