/**
 * @file kernel/test/test_socket.cpp
 * @brief In-kernel tests for the F7-M6 socket syscall plumbing (batch 1b).
 *
 * B1b ships the fd + syscall machinery with a STUB Socket: socket()/close()
 * work, and bind/connect/listen/accept/sendto/recvfrom return -ENOSYS until B2
 * (UdpSocket) / B3 (TcpSocket) override the virtuals.  These tests prove the
 * plumbing -- socket() allocates a real fd backed by a SocketOps inode, arg
 * validation rejects a bad family/type, the fd resolves back to the Socket, and
 * the stub send() is reached.  Real send/recv + loopback echo land with B2/B3.
 *
 * The stub Socket needs no net stack, so this runs in the test kernel (which
 * does not bring up production net_init) -- the create_socket() factory returns
 * a bare Socket for AF_INET regardless of g_ready in B1b.
 */

#include <stdint.h>

#include "kernel/errno.hpp"               // kEafnosupport / kEprotonosupport
#include "kernel/fs/file.hpp"             // File
#include "kernel/fs/vfs_mount.hpp"        // current_fd_table()
#include "kernel/net/socket.hpp"          // Socket / socket_ops / kAfInet / kSock*
#include "kernel/syscall/sys_close.hpp"   // sys_close
#include "kernel/syscall/sys_socket.hpp"  // sys_socket
#include "kernel/test/big_kernel_test.h"

using cinux::fs::current_fd_table;
using cinux::net::Socket;
using cinux::net::kAfInet;
using cinux::net::kSockDgram;
using cinux::net::kSockStream;
using cinux::net::socket_ops;
using cinux::syscall::sys_close;
using cinux::syscall::sys_socket;

namespace test_socket {

static constexpr uint64_t kFiller = 0;

void test_socket_dgram_returns_fd() {
    int64_t fd = sys_socket(kAfInet, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
}

void test_socket_stream_returns_fd() {
    int64_t fd = sys_socket(kAfInet, kSockStream, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
}

void test_socket_rejects_bad_family() {
    // AF_UNIX (1) -- only AF_INET is supported.
    int64_t r = sys_socket(1, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_EQ(r, -cinux::kEafnosupport);
}

void test_socket_rejects_bad_type() {
    // SOCK_RAW (3) -- only STREAM / DGRAM are supported.
    int64_t r = sys_socket(kAfInet, 3, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_EQ(r, -cinux::kEprotonosupport);
}

void test_socket_fd_routes_to_socket_stub() {
    int64_t fd = sys_socket(kAfInet, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);

    cinux::fs::File* f = current_fd_table().get(static_cast<int>(fd));
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(f->inode);
    // The fd's inode ops IS the shared SocketOps -> it is a socket fd.
    TEST_ASSERT_EQ(f->inode->ops, &socket_ops());

    auto* s = static_cast<Socket*>(f->inode->fs_private);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ(s->type(), kSockDgram);

    // Stub: send returns NotImplemented (B2/B3 override with real protocol).
    uint8_t b = 0;
    auto r = s->send(&b, 0);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_TRUE(r.error() == cinux::lib::Error::NotImplemented);

    // close() frees the File (Socket/Inode share the pipe-style hobby leak).
    TEST_ASSERT_EQ(sys_close(static_cast<uint64_t>(fd), kFiller, kFiller, kFiller, kFiller, kFiller),
                   0);
}

}  // namespace test_socket

extern "C" void run_socket_tests() {
    TEST_SECTION("F7-M6 socket (B1b plumbing)");
    RUN_TEST(test_socket::test_socket_dgram_returns_fd);
    RUN_TEST(test_socket::test_socket_stream_returns_fd);
    RUN_TEST(test_socket::test_socket_rejects_bad_family);
    RUN_TEST(test_socket::test_socket_rejects_bad_type);
    RUN_TEST(test_socket::test_socket_fd_routes_to_socket_stub);
    TEST_SUMMARY();
}
