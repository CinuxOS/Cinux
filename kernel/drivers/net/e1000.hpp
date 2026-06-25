/**
 * @file kernel/drivers/net/e1000.hpp
 * @brief Intel e1000 (8254x) NIC driver -- PCI discovery + reset + EEPROM MAC
 *
 * 批a scope: discover the NIC via PCI (vendor 0x8086), enable Bus Master +
 * Memory Space, map BAR0 into the MMIO window, reset the controller, disable
 * interrupts, force the link up, and read the EEPROM MAC address.  No rings or
 * interrupts yet -- the legacy RX ring + polled receive land in 批b, and the
 * MSI interrupt path is deferred (82540em is MSI, not MSI-X).
 *
 * Modelled on XHCIController (instance + singleton, init(const PCIDevice&)).
 * The e1000 register block is sparse, so access is reg_read / reg_write at the
 * named offsets in e1000_mmio.hpp rather than a packed struct.
 *
 * Namespace: cinux::drivers::net
 */

#pragma once

#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/drivers/pci/pci.hpp"

namespace cinux::drivers::net {

class E1000Controller {
public:
    static E1000Controller& instance();
    static void             set_instance(E1000Controller* c);
    /// Null-safe probe: true once init() installed the singleton.
    static bool             has_controller() { return s_instance_ != nullptr; }

    /**
     * @brief Bring up the NIC from a PCI descriptor
     *
     * Enables PCI Bus Master + Memory Space, maps BAR0 (32-bit MMIO) into the
     * MMIO window, software-resets the controller (non-fatal if the reset bit
     * is slow to self-clear under QEMU), disables interrupts, asserts Set Link
     * Up, and reads the EEPROM MAC.  Leaves the NIC ready for RX ring setup.
     */
    cinux::lib::ErrorOr<void> init(const pci::PCIDevice& dev);

    /// STATUS.LU (informational; under QEMU the link is up once SLU is set).
    bool link_up() const;

    /// True once BAR0 is mapped (init() succeeded).
    bool present() const { return mmio_base_ != 0; }

    /// Copy the EEPROM MAC (6 octets) into @p out.
    void mac(uint8_t out[6]) const;

private:
    /// Read a 32-bit device register at @p offset (bytes from BAR0).
    uint32_t reg_read(uint32_t offset) const;
    /// Write a 32-bit device register at @p offset (bytes from BAR0).
    void     reg_write(uint32_t offset, uint32_t value);

    /// Read one 16-bit EEPROM word at word-address @p addr via the EERD poll.
    /// Returns 0xFFFF if the DONE bit never asserts (bounded poll).
    uint16_t read_eeprom(uint8_t addr);

    /// Read EEPROM words 0..2 and assemble the 6 MAC octets into mac_.
    void read_mac();

    uint64_t       mmio_base_ = 0;  ///< BAR0 kernel-virtual base (0 = unmapped)
    uint8_t        mac_[6]    = {};
    pci::PCIDevice dev_{};  ///< retained for later MSI / capability use

    static E1000Controller* s_instance_;
};

}  // namespace cinux::drivers::net
