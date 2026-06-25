/**
 * @file kernel/drivers/net/e1000.cpp
 * @brief e1000 NIC bring-up: PCI enable + BAR0 map + reset + EEPROM MAC
 *
 * 批a.  No RX/TX rings or interrupts yet.  The EEPROM MAC read + link status are
 * verified in QEMU (run-kernel-test with -device e1000): find_e1000 finds the
 * 82540em, init maps BAR0, resets, reads MAC=52:54:00:..., reports link=1.
 *
 * Namespace: cinux::drivers::net
 */

#include "e1000.hpp"

#include <stdint.h>

#include "e1000_mmio.hpp"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pci/pci_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::drivers::net {

E1000Controller* E1000Controller::s_instance_ = nullptr;

E1000Controller& E1000Controller::instance() {
    return *s_instance_;
}

void E1000Controller::set_instance(E1000Controller* c) {
    s_instance_ = c;
}

namespace {
// MMIO sub-allocation inside the KMEM_MMIO window.  In use: AHCI @+0x0, LAPIC
// @+0x10000, IOAPIC @+0x11000, xHCI BAR0 @+0x20000, MSI-X Table @+0x40000, PBA
// @+0x41000.  e1000 BAR0 (32-bit MMIO) slots at +0x50000 -- clear of the MSI-X
// block and far under the 2 MB ceiling.  8 pages (32 KB) cover every register
// 批a/批b touches (CTRL 0x0 .. MTA 0x5200 / RAL0 0x5400).
constexpr uint64_t kE1000MmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x50000;
constexpr uint64_t kMmioFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
constexpr uint64_t kMmioPages = 8;
constexpr uint64_t kPageSize  = 4096;
constexpr uint32_t kPollIters = 100000;  // bounded poll cap (QEMU completes instantly)
}  // namespace

uint32_t E1000Controller::reg_read(uint32_t offset) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio_base_ + offset);
}

void E1000Controller::reg_write(uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(mmio_base_ + offset) = value;
}

uint16_t E1000Controller::read_eeprom(uint8_t addr) {
    reg_write(e1000reg::EERD,
              (static_cast<uint32_t>(addr) << e1000reg::EERD_ADDR_SHIFT) | e1000reg::EERD_START);
    for (uint32_t i = 0; i < kPollIters; ++i) {
        if (reg_read(e1000reg::EERD) & e1000reg::EERD_DONE) {
            return static_cast<uint16_t>((reg_read(e1000reg::EERD) >> e1000reg::EERD_DATA_SHIFT) &
                                         0xFFFF);
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    return 0xFFFF;  // DONE never asserted
}

void E1000Controller::read_mac() {
    const uint16_t w0 = read_eeprom(0);
    const uint16_t w1 = read_eeprom(1);
    const uint16_t w2 = read_eeprom(2);
    mac_[0]           = static_cast<uint8_t>(w0 & 0xFF);
    mac_[1]           = static_cast<uint8_t>((w0 >> 8) & 0xFF);
    mac_[2]           = static_cast<uint8_t>(w1 & 0xFF);
    mac_[3]           = static_cast<uint8_t>((w1 >> 8) & 0xFF);
    mac_[4]           = static_cast<uint8_t>(w2 & 0xFF);
    mac_[5]           = static_cast<uint8_t>((w2 >> 8) & 0xFF);
}

cinux::lib::ErrorOr<void> E1000Controller::init(const pci::PCIDevice& dev) {
    dev_ = dev;

    // 1. Enable PCI Bus Master (DMA) + Memory Space (MMIO response).
    const uint32_t cmd = pci::PCI::pci_read(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND);
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func, pci::PciReg::COMMAND,
                        cmd | pci::PciCmd::BUS_MASTER | pci::PciCmd::MEM_SPACE);

    // 2. Map BAR0 (32-bit MMIO) into the MMIO window.
    const uint64_t bar0 = dev.bar[0];
    for (uint64_t i = 0; i < kMmioPages; ++i) {
        if (!cinux::mm::g_vmm.map(kE1000MmioVirt + i * kPageSize, bar0 + i * kPageSize,
                                  kMmioFlags)) {
            return cinux::lib::Error::OutOfMemory;
        }
    }
    mmio_base_ = kE1000MmioVirt;

    // 3. Software reset (CTRL.RST self-clears).  Bounded poll; non-fatal -- the
    //    EEPROM MAC read + link check are the 批a deliverables, and QEMU resets
    //    instantly.  Real HW completes in well under 1 ms.
    reg_write(e1000reg::CTRL, reg_read(e1000reg::CTRL) | e1000reg::CTRL_RST);
    bool rst_done = false;
    for (uint32_t i = 0; i < kPollIters; ++i) {
        if (!(reg_read(e1000reg::CTRL) & e1000reg::CTRL_RST)) {
            rst_done = true;
            break;
        }
        if (i > 0 && (i % 10000) == 0) {
            cinux::proc::Scheduler::yield();
        }
    }
    if (!rst_done) {
        cinux::lib::kprintf("[e1000] reset did not self-clear (CTRL=0x%x) -- continuing\n",
                            reg_read(e1000reg::CTRL));
    }

    // 4. Disable interrupts and force the link up.
    reg_write(e1000reg::IMC, 0xFFFFFFFF);
    reg_write(e1000reg::CTRL, reg_read(e1000reg::CTRL) | e1000reg::CTRL_SLU);

    // 5. Read the EEPROM MAC.
    read_mac();

    const uint32_t status = reg_read(e1000reg::STATUS);
    cinux::lib::kprintf("[e1000] BAR0=0x%lx MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%d FD=%d\n",
                        static_cast<unsigned long>(bar0), mac_[0], mac_[1], mac_[2], mac_[3],
                        mac_[4], mac_[5], static_cast<int>((status & e1000reg::STATUS_LU) != 0),
                        static_cast<int>((status & e1000reg::STATUS_FD) != 0));

    return {};
}

bool E1000Controller::link_up() const {
    if (mmio_base_ == 0) {
        return false;
    }
    return (reg_read(e1000reg::STATUS) & e1000reg::STATUS_LU) != 0;
}

void E1000Controller::mac(uint8_t out[6]) const {
    for (int i = 0; i < 6; ++i) {
        out[i] = mac_[i];
    }
}

}  // namespace cinux::drivers::net
