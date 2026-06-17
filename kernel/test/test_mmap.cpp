/**
 * @file kernel/test/test_mmap.cpp
 * @brief QEMU in-kernel tests for sys_mmap (F2-M2 batch 1)
 *
 * Anonymous mmap via the syscall interface.  The big-kernel-test environment
 * has no scheduler loop (Scheduler::current() is nullptr), so each case
 * installs a temporary Task pointing at a fresh AddressSpace -- the same trick
 * test_fork_exec uses.  No physical pages are mapped (lazy mmap); we verify the
 * VMA bookkeeping, not demand paging.
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_mmap.hpp"

using cinux::arch::USER_MMAP_BASE;
using cinux::syscall::MAP_ANONYMOUS;
using cinux::syscall::MAP_FIXED;
using cinux::syscall::MAP_PRIVATE;
using cinux::syscall::PROT_READ;
using cinux::syscall::PROT_WRITE;
using cinux::syscall::sys_mmap;

namespace {

/// RAII: install @p task as current, restore the previous on destruction.
struct CurrentTaskGuard {
    cinux::proc::Task* prev;
    explicit CurrentTaskGuard(cinux::proc::Task* task) : prev(cinux::proc::Scheduler::current()) {
        cinux::proc::Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { cinux::proc::Scheduler::set_current(prev); }
};

constexpr uint64_t kProtRw   = PROT_READ | PROT_WRITE;
constexpr uint64_t kPrivAnon = MAP_PRIVATE | MAP_ANONYMOUS;

}  // namespace

// ============================================================
// Test 1: anonymous mmap registers a VMA in the mmap window
// ============================================================

namespace test_mmap_anon {

void test_anon_mmap_registers_vma() {
    cinux::mm::AddressSpace as;
    cinux::proc::Task       tmp{};
    tmp.addr_space = &as;
    CurrentTaskGuard guard(&tmp);

    int64_t r = sys_mmap(0, 4096, kProtRw, kPrivAnon, 0, 0);
    TEST_ASSERT_TRUE(r > 0);
    TEST_ASSERT_TRUE(static_cast<uint64_t>(r) >= USER_MMAP_BASE);

    cinux::mm::VMA* v = as.vmas().find(static_cast<uint64_t>(r));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->start == static_cast<uint64_t>(r));
    TEST_ASSERT_TRUE(v->end == static_cast<uint64_t>(r) + 4096);
    TEST_ASSERT_TRUE(v->flags == (cinux::mm::VmaFlags::Anonymous | cinux::mm::VmaFlags::Read |
                                  cinux::mm::VmaFlags::Write));
}

}  // namespace test_mmap_anon

// ============================================================
// Test 2: two mappings land at distinct, non-overlapping addresses
// ============================================================

namespace test_mmap_distinct {

void test_two_mappings_distinct() {
    cinux::mm::AddressSpace as;
    cinux::proc::Task       tmp{};
    tmp.addr_space = &as;
    CurrentTaskGuard guard(&tmp);

    int64_t a = sys_mmap(0, 4096, PROT_READ, kPrivAnon, 0, 0);
    int64_t b = sys_mmap(0, 8192, kProtRw, kPrivAnon, 0, 0);
    TEST_ASSERT_TRUE(a > 0);
    TEST_ASSERT_TRUE(b > 0);
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_TRUE(as.vmas().count() == 2);
}

}  // namespace test_mmap_distinct

// ============================================================
// Test 3: MAP_FIXED honours the address; unaligned addr rejected
// ============================================================

namespace test_mmap_fixed {

void test_map_fixed_and_unaligned() {
    cinux::mm::AddressSpace as;
    cinux::proc::Task       tmp{};
    tmp.addr_space = &as;
    CurrentTaskGuard guard(&tmp);

    int64_t r = sys_mmap(USER_MMAP_BASE, 4096, kProtRw, kPrivAnon | MAP_FIXED, 0, 0);
    TEST_ASSERT_TRUE(r == static_cast<int64_t>(USER_MMAP_BASE));

    // Unaligned fixed address is rejected (EINVAL).
    int64_t bad = sys_mmap(USER_MMAP_BASE + 1, 4096, PROT_READ, kPrivAnon | MAP_FIXED, 0, 0);
    TEST_ASSERT_TRUE(bad < 0);
}

}  // namespace test_mmap_fixed

// ============================================================
// Test 4: invalid arguments are rejected
// ============================================================

namespace test_mmap_reject {

void test_invalid_args() {
    cinux::mm::AddressSpace as;
    cinux::proc::Task       tmp{};
    tmp.addr_space = &as;
    CurrentTaskGuard guard(&tmp);

    // length == 0 -> EINVAL
    TEST_ASSERT_TRUE(sys_mmap(0, 0, PROT_READ, kPrivAnon, 0, 0) < 0);

    // neither MAP_SHARED nor MAP_PRIVATE -> EINVAL
    TEST_ASSERT_TRUE(sys_mmap(0, 4096, PROT_READ, MAP_ANONYMOUS, 0, 0) < 0);

    // non-anonymous (file mapping) not yet supported -> ENOSYS
    TEST_ASSERT_TRUE(sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, 0, 0) < 0);
}

}  // namespace test_mmap_reject

// ============================================================
// Entry point
// ============================================================

extern "C" void run_mmap_tests() {
    TEST_SECTION("mmap Tests (F2-M2-1)");

    RUN_TEST(test_mmap_anon::test_anon_mmap_registers_vma);
    RUN_TEST(test_mmap_distinct::test_two_mappings_distinct);
    RUN_TEST(test_mmap_fixed::test_map_fixed_and_unaligned);
    RUN_TEST(test_mmap_reject::test_invalid_args);

    TEST_SUMMARY();
}
