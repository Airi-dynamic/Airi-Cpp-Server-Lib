#pragma once

#include <cassert>

#define DISALLOW_COPY(cname)                                                                       \
    cname(const cname &) = delete;                                                                 \
    cname &operator=(const cname &) = delete;

#define DISALLOW_MOVE(cname)                                                                       \
    cname(cname &&) = delete;                                                                      \
    cname &operator=(cname &&) = delete;

#define DISALLOW_COPY_AND_MOVE(cname)                                                              \
    DISALLOW_COPY(cname);                                                                          \
    DISALLOW_MOVE(cname);

#define ASSERT(expr, message) assert((expr) && (message))

// 函数返回码，替代裸 void 或 bool 返回
enum RC {
    RC_UNDEFINED,
    RC_SUCCESS,
    RC_SOCKET_ERROR,
    RC_POLLER_ERROR,
    RC_CONNECTION_ERROR,
    RC_ACCEPTOR_ERROR,
    RC_UNIMPLEMENTED
};