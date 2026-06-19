/**
 * @file kernel/drivers/acpi/madt.cpp
 * @brief MADT (APIC) table parser (F4-M1)
 *
 * Walks the MADT's Interrupt Controller Structure list and decodes the fields
 * M2 needs: Local APIC base, the enabled CPU APIC IDs, the first I/O APIC
 * base/gsi_base, the PC-AT-compat flag, and the ISA IRQ source overrides.
 *
 * ICS entries are variable-length and may start at any offset, so each entry
 * is read through its [[gnu::packed]] struct (the compiler then emits
 * alignment-safe field accesses).
 */

#include <stddef.h>
#include <stdint.h>

#include "acpi.hpp"

namespace cinux::drivers::acpi {

namespace {

/// Flags bit 0 of a Processor Local APIC entry: the CPU is enabled.
constexpr uint32_t kLocalApicEnabled       = 0x01;
/// Flags bit 1 (ACPI 6.3+): online-capable -- treat like enabled for bring-up.
constexpr uint32_t kLocalApicOnlineCapable = 0x02;

}  // namespace

ACPIInfo parse_madt(const SDTHeader* madt) {
    ACPIInfo info{};
    if (madt == nullptr || madt->length < sizeof(MADTHeader)) {
        return info;
    }

    const auto* m           = reinterpret_cast<const MADTHeader*>(madt);
    info.local_apic_address = m->local_apic_address;
    info.has_pcat_compat    = (m->flags & kMadtPcatCompat) != 0;

    const uint8_t* p   = reinterpret_cast<const uint8_t*>(madt) + sizeof(MADTHeader);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(madt) + madt->length;

    while (p + sizeof(ICSHeader) <= end) {
        const auto* ics = reinterpret_cast<const ICSHeader*>(p);
        if (ics->length < sizeof(ICSHeader) || p + ics->length > end) {
            break;  // malformed entry -- stop scanning
        }

        switch (ics->type) {
        case kIcsProcessorLocalApic: {
            const auto* e = reinterpret_cast<const ProcessorLocalAPICEntry*>(p);
            const bool  enabled =
                (e->flags & kLocalApicEnabled) != 0 || (e->flags & kLocalApicOnlineCapable) != 0;
            if (enabled && info.cpu_count < kMaxCpus) {
                info.cpu_apic_ids[info.cpu_count++] = e->apic_id;
            }
            break;
        }
        case kIcsIoapic: {
            const auto* e = reinterpret_cast<const IOAPICEntry*>(p);
            if (!info.has_ioapic) {  // record the first I/O APIC only
                info.ioapic_address  = e->ioapic_address;
                info.ioapic_gsi_base = e->gsi_base;
                info.has_ioapic      = true;
            }
            break;
        }
        case kIcsInterruptSourceOverride: {
            const auto* e = reinterpret_cast<const InterruptSourceOverrideEntry*>(p);
            if (info.irq_override_count < 16) {
                info.irq_overrides[info.irq_override_count++] =
                    ACPIInfo::IrqOverride{e->source_irq, e->global_irq, e->flags};
            }
            break;
        }
        default:
            break;  // skip NMI / LAPIC NMI / x2APIC / etc.
        }

        p += ics->length;
    }

    return info;
}

}  // namespace cinux::drivers::acpi
