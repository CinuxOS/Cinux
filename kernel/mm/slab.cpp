/**
 * @file kernel/mm/slab.cpp
 * @brief SLAB small-object allocator implementation (F2-M7b batch 1)
 */

#include "kernel/mm/slab.hpp"

#include <stddef.h>
#include <stdint.h>

#include <cinux/numeric.hpp>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

// ============================================================
// Global instance
// ============================================================

SlabAllocator g_slab;

// ============================================================
// Helpers
// ============================================================

namespace {

using cinux::lib::align_up;

constexpr uint64_t kPageFlags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;

void memzero(void* start, size_t len) {
    auto* p = static_cast<uint8_t*>(start);
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

}  // namespace

// ============================================================
// SlabAllocator::init
// ============================================================

void SlabAllocator::init(uint64_t virt_base, uint64_t region_size) {
    slab_base_  = virt_base;
    slab_brk_   = virt_base;
    slab_end_   = virt_base + region_size;
    slab_pages_ = 0;

    for (int i = 0; i < kNumCaches; i++) {
        SlabCache& c   = caches_[i];
        c.obj_size      = kSlabMinObj << i;
        uint64_t hdr    = align_up(sizeof(SlabHeader), c.obj_size);
        c.objs_per_slab = static_cast<uint16_t>((cinux::arch::PAGE_SIZE - hdr) / c.obj_size);
        c.name          = "general";
        c.partial       = nullptr;
        c.full          = nullptr;
        c.slab_count    = 0;
        c.ctor          = nullptr;
        c.dtor          = nullptr;
    }

    cinux::lib::kprintf("[SLAB] Initialised: %d general caches (%u..%u B), window %u MB\n",
                        kNumCaches, static_cast<unsigned>(kSlabMinObj),
                        static_cast<unsigned>(kSlabMaxObj),
                        static_cast<unsigned>(region_size / (1024 * 1024)));
}

// ============================================================
// size_to_index: smallest general cache that holds @p size, or -1
// ============================================================

int SlabAllocator::size_to_index(size_t size) {
    if (size <= kSlabMinObj) {
        return 0;
    }
    if (size > kSlabMaxObj) {
        return -1;
    }
    size_t s   = kSlabMinObj;
    int    idx = 0;
    while (s < size) {
        s <<= 1;
        idx++;
    }
    return idx < kNumCaches ? idx : -1;
}

// ============================================================
// Doubly-linked list ops over SlabHeader (next/prev)
// ============================================================

void SlabAllocator::list_push_front(SlabHeader** head, SlabHeader* s) {
    s->prev = nullptr;
    s->next = *head;
    if (*head != nullptr) {
        (*head)->prev = s;
    }
    *head = s;
}

void SlabAllocator::list_remove(SlabHeader** head, SlabHeader* s) {
    if (s->prev != nullptr) {
        s->prev->next = s->next;
    } else {
        *head = s->next;
    }
    if (s->next != nullptr) {
        s->next->prev = s->prev;
    }
    s->next = nullptr;
    s->prev = nullptr;
}

// ============================================================
// grow_cache: map one fresh slab page and build its free list
// ============================================================

SlabHeader* SlabAllocator::grow_cache(SlabCache& cache) {
    if (slab_brk_ + cinux::arch::PAGE_SIZE > slab_end_) {
        cinux::lib::kprintf("[SLAB] OOM: virtual window exhausted\n");
        return nullptr;
    }
    uint64_t virt = slab_brk_;
    uint64_t phys = g_pmm.alloc_page();
    if (phys == 0) {
        return nullptr;
    }
    if (!g_vmm.map(virt, phys, kPageFlags)) {
        g_pmm.free_page(phys);
        return nullptr;
    }

    slab_brk_ += cinux::arch::PAGE_SIZE;
    slab_pages_++;

    auto* s  = reinterpret_cast<SlabHeader*>(virt);
    s->cache = &cache;
    s->inuse = 0;
    s->total = cache.objs_per_slab;
    s->next  = nullptr;
    s->prev  = nullptr;

    // Build the intrusive free list: object[i].link = &object[i+1].
    uint64_t hdr_area = align_up(sizeof(SlabHeader), cache.obj_size);
    uint64_t base     = virt + hdr_area;
    s->freelist       = reinterpret_cast<void*>(base);
    for (uint16_t i = 0; i < cache.objs_per_slab; i++) {
        void** slot = reinterpret_cast<void**>(base + i * cache.obj_size);
        *slot       = (i + 1 < cache.objs_per_slab)
                        ? reinterpret_cast<void*>(base + (i + 1) * cache.obj_size)
                        : nullptr;
    }

    list_push_front(&cache.partial, s);
    cache.slab_count++;
    return s;
}

// ============================================================
// alloc_locked (caller holds the spinlock / interrupts disabled)
// ============================================================

void* SlabAllocator::alloc_locked(size_t size) {
    if (size == 0) {
        return nullptr;
    }
    int idx = size_to_index(size);
    if (idx < 0) {
        return nullptr;  // not slab-eligible
    }

    SlabCache&  c = caches_[idx];
    SlabHeader* s = c.partial;
    if (s == nullptr) {
        s = grow_cache(c);
        if (s == nullptr) {
            return nullptr;
        }
    }

    void* obj = s->freelist;
    // Pop the head: the object's first word holds the next free pointer.
    s->freelist = *reinterpret_cast<void**>(obj);
    s->inuse++;

    if (s->inuse == s->total) {
        list_remove(&c.partial, s);
        list_push_front(&c.full, s);
    }

    memzero(obj, c.obj_size);  // match Heap::alloc's zeroing; also clears the link word
    return obj;
}

// ============================================================
// free_locked (caller holds the spinlock / interrupts disabled)
// ============================================================

void SlabAllocator::free_locked(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    // Strong guard: only pointers inside the slab window are slab objects.
    if (p < slab_base_ || p >= slab_brk_) {
        return;
    }

    // The owning slab page is the page-aligned base of the pointer.
    auto* s = reinterpret_cast<SlabHeader*>(p & ~static_cast<uintptr_t>(cinux::arch::PAGE_SIZE - 1));
    if (s->cache == nullptr) {
        return;  // not an initialised slab page
    }
    SlabCache* c = s->cache;

    bool was_full = (s->inuse == s->total);
    // Push back onto the intrusive free list.
    *reinterpret_cast<void**>(ptr) = s->freelist;
    s->freelist                    = ptr;
    s->inuse--;

    if (was_full) {
        list_remove(&c->full, s);
        list_push_front(&c->partial, s);
    }
}

// ============================================================
// Public alloc / free (IRQ-safe spinlock)
// ============================================================

void* SlabAllocator::alloc(size_t size) {
    auto g = lock_.irq_guard();
    (void)g;
    return alloc_locked(size);
}

void SlabAllocator::free(void* ptr) {
    auto g = lock_.irq_guard();
    (void)g;
    free_locked(ptr);
}

}  // namespace cinux::mm
