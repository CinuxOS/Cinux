/**
 * @file kernel/syscall/sys_read.cpp
 * @brief sys_read handler implementation
 *
 * For fd=0 (stdin): reads keyboard input from the PS/2 ring buffer.
 * For other fds: reads through VFS (fd -> File -> Inode -> ops -> read).
 */

#include "kernel/syscall/sys_read.hpp"

#include <stdint.h>

#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/tty/console_tty.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/mm/page_cache.hpp"

namespace cinux::syscall {

namespace {

/// Maximum iterations to spin-wait for a key before returning 0
constexpr uint32_t SPIN_WAIT_ITERS = 1'000'000;

}  // anonymous namespace

int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    if (buf_virt == 0) {
        return -kEfault;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -kEfault;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -kEfault;
    }

    // Check FDTable first -- if the fd has a valid VFS entry (e.g. pipe),
    // use the VFS read path regardless of fd number.
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        auto* buf = reinterpret_cast<void*>(buf_virt);
        auto  g   = file->offset_lock_.guard();
        (void)g;
        // Disk-backed files (ext2) are served through the PageCache so that
        // read() and demand paging share one cached copy; pipes and other
        // transient ops keep their direct read() path (is_page_cacheable is
        // false by default, so behaviour is unchanged for them).
        auto read_result =
            file->inode->ops->is_page_cacheable()
                ? cinux::mm::g_page_cache.read_bytes(file->inode, file->offset, buf, count)
                : file->inode->ops->read(file->inode, file->offset, buf, count);
        if (!read_result.ok()) {
            return -to_errno(read_result.error());
        }
        if (read_result.value() > 0) {
            file->offset += static_cast<uint64_t>(read_result.value());
        }
        return read_result.value();
    }

    // fd=0 (stdin): read a cooked line through the console TTY line discipline
    // (F10-M3). The keyboard IRQ feeds input_char(); we drain read_cooked().
    // Spin-wait for a line (batch 3 replaces this with a proper block); if
    // nothing arrives, return 0 like the legacy path so headless tests that
    // poll stdin don't hang.
    if (fd == 0) {
        auto* buf = reinterpret_cast<char*>(buf_virt);

        uint64_t n = cinux::drivers::console_tty().read_cooked(buf, count);
        if (n > 0) {
            return static_cast<int64_t>(n);
        }
        for (uint32_t i = 0; i < SPIN_WAIT_ITERS; i++) {
            __asm__ volatile("pause");
            n = cinux::drivers::console_tty().read_cooked(buf, count);
            if (n > 0) {
                return static_cast<int64_t>(n);
            }
        }
        return 0;
    }

    // No VFS entry and not a legacy fd -- fail
    return -kEbadf;
}

}  // namespace cinux::syscall
