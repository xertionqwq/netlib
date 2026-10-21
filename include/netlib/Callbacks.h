#ifndef NETLIB_CALLBACKS_H
#define NETLIB_CALLBACKS_H

#include <functional>
#include <memory>

namespace netlib {

class TcpConnection;
class Buffer;
class Timestamp;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;
using CloseCallback = std::function<void(const TcpConnectionPtr &)>;
using WriteCompleteCallback = std::function<void(const TcpConnection &)>;

using MessageCallback = std::function<void(const TcpConnection &,
                                           Buffer *,
                                           Timestamp)>;

} // namespace netlib

#endif // NETLIB_CALLBACKS_H
