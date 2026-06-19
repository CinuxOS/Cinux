/**
 * @file kernel/drivers/acpi/acpi.hpp
 * @brief ACPI static-table discovery and structures (F4-M1)
 *
 * Minimal ACPI support for SMP bring-up: locate the RSDP, then (M1-2) walk the
 * XSDT/RSDT to find the MADT, FADT and HPET tables.  There is deliberately no
 * AML interpreter -- only the static tables the kernel needs: the Local APIC
 * base address, the CPU APIC ID list, the I/O APIC base, and the IRQ source
 * overrides.
 *
 * All tables live in conventional memory (the RSDP below 1 MB, the SDTs
 * anywhere in low RAM) and are reached through the mini loader's identity
 * direct map via phys_to_virt().  Unlike the APIC MMIO region, these are
 * ordinary RAM so the cache-enabled direct map is correct here.
 *
 * Namespace: cinux::drivers::acpi
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::drivers::acpi {

/// RSDP signature, 8 bytes: "RSD PTR " (two trailing spaces).
constexpr char kRsdpSignature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};

/// ACPI 2.0 Root System Description Pointer.
///
/// The first 20 bytes are the ACPI 1.0 layout, checksummed by @ref checksum.
/// The remaining fields are ACPI 2.0+ and are covered by @ref extended_checksum
/// over @ref length bytes.
struct [[gnu::packed]] RSDP {
    char     signature[8];  ///< "RSD PTR "
    uint8_t  checksum;      ///< byte sum of [0, 20) == 0
    char     oem_id[6];
    uint8_t  revision;           ///< 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;       ///< RSDT physical address (32-bit)
    uint32_t length;             ///< total RSDP length (36 for 2.0+)
    uint64_t xsdt_address;       ///< XSDT physical address (64-bit)
    uint8_t  extended_checksum;  ///< byte sum of [0, length) == 0
    uint8_t  reserved[3];
};
static_assert(sizeof(RSDP) == 36, "ACPI 2.0 RSDP must be 36 bytes");

/// Common header of every ACPI System Description Table (MADT, FADT, HPET...).
///
/// Defined here in M1-1 even though table walking arrives in M1-2: the layout
/// is the contract every SDT shares, and the RSDP tests already need it to be
/// stable.
struct [[gnu::packed]] SDTHeader {
    char     signature[4];  ///< e.g. "APIC", "FACP", "HPET"
    uint32_t length;        ///< whole table length, header included
    uint8_t  revision;
    uint8_t  checksum;  ///< byte sum of all bytes over length == 0
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};
static_assert(sizeof(SDTHeader) == 36, "ACPI SDT header must be 36 bytes");

/// Validate an ACPI checksum: the byte sum over the first @p len bytes must be
/// 0 (mod 256).
///
/// @param table  Pointer to the table (already mapped via the direct map).
/// @param len    Number of bytes to sum.
/// @return       true if the checksum is valid.
bool validate_checksum(const void* table, size_t len);

/// Locate the RSDP.
///
/// Searches the EBDA (segment pointer at physical 0x40E, first 1 KB) then the
/// BIOS ROM area 0xE0000-0xFFFFF in 16-byte strides.  Both the ACPI 1.0
/// checksum (first 20 bytes) and, for revision >= 2, the extended checksum
/// (whole table) are verified before the pointer is returned.
///
/// @return  RSDP via the direct map, or nullptr if not found / invalid.
const RSDP* find_rsdp();

}  // namespace cinux::drivers::acpi
