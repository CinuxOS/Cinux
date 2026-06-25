/**
 * @file kernel/test/test_e1000.cpp
 * @brief QEMU in-kernel integration tests for the e1000 NIC (F5-M6 批a)
 *
 * Runs inside the big kernel test suite.  PCI-enumerates an Intel e1000 NIC
 * (vendor 0x8086); if none is present (the default QEMU config has no e1000)
 * the test SKIPS (no failure).  Under run-kernel-test (-device e1000 ...) it
 * exercises the real bring-up: PCI find + BAR0 map + reset + EEPROM MAC read,
 * asserting the MAC is non-zero and reporting link status.
 *
 * Preconditions: VMM initialised (g_vmm.init in main_test.cpp) for the BAR0 map.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/net/e1000.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::drivers::net::E1000Controller;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::pci::PCI;

// ============================================================
// Test 1: find e1000 + read EEPROM MAC (skip if no NIC present)
// ============================================================

namespace test_e1000 {

void test_detect_and_mac() {
    PCI pci;
    pci.init();

    PCIDevice dev{};
    if (!pci.find_e1000(dev)) {
        cinux::lib::kprintf("[e1000] no NIC present -- skipping detect/MAC test\n");
        return;  // counts as a pass when no e1000 is attached
    }

    E1000Controller           nic;
    cinux::lib::ErrorOr<void> r = nic.init(dev);
    TEST_ASSERT_TRUE(r.ok());

    uint8_t mac[6] = {};
    nic.mac(mac);
    cinux::lib::kprintf("[e1000] EEPROM MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%d\n", mac[0],
                        mac[1], mac[2], mac[3], mac[4], mac[5], static_cast<int>(nic.link_up()));

    // A real NIC has a non-zero MAC (QEMU assigns 52:54:00:...).  Reject the
    // all-zero "no EEPROM" read so a silent EERD failure fails loudly.
    bool mac_nonzero = false;
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0) {
            mac_nonzero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(mac_nonzero);

    E1000Controller::set_instance(&nic);
}

}  // namespace test_e1000

extern "C" void run_e1000_tests() {
    TEST_SECTION("e1000");
    RUN_TEST(test_e1000::test_detect_and_mac);
    TEST_SUMMARY();
}
