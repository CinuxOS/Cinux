/**
 * @file kernel/gui/visor_core/visor_host_cinux.cpp
 * @brief Cinux host adapter -- fills visor_host for the in-kernel desktop
 *
 * See visor_host_cinux.hpp. Every callback forwards to an existing in-tree
 * facility; no new behaviour is introduced. The poll_event callback is the
 * only non-trivial one: it dequeues a cinux::gui::Event from the unified mouse
 * queue and serialises it into a visor_event_header + typed payload so the
 * (host-agnostic) visor_pump body can consume it.
 *
 * Compile condition: CINUX_GUI.
 */

#include "visor_host_cinux.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/drivers/mouse.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/video/framebuffer.hpp"  // Framebuffer (flush target)
#include "kernel/gui/event.hpp"
#include "kernel/gui/gui_init.hpp"  // create_shell_terminal
#include "kernel/lib/kprintf.hpp"   // kvprintf / kprintf
#include "kernel/lib/string.hpp"    // memcpy
#include "kernel/mm/slab.hpp"       // kmalloc / kfree
#include "visor_event.h"
#include "visor_event_payload.h"

namespace cinux::gui {
namespace {

visor_host_desktop           g_cinux_desktop{};
visor_host                   g_cinux_host{};
cinux::drivers::Framebuffer* g_fb = nullptr; /* flush forwards dirty rects here (§4c) */

/* ============================================================
 * L2 Input: dequeue one cinux::gui::Event and serialise it to a visor event.
 * ============================================================ */
bool cinux_poll_event(void* ctx, visor_event_header* out, uint16_t out_cap) {
    (void)ctx;
    if (out == nullptr || out_cap < sizeof(visor_event_header)) {
        return false;
    }

    Event ev;
    if (!cinux::drivers::Mouse::event_queue().dequeue(ev)) {
        return false;
    }

    out->magic   = VISOR_EVENT_MAGIC;
    out->version = VISOR_ABI_VERSION;
    out->flags   = 0;

    uint8_t* tail  = reinterpret_cast<uint8_t*>(out) + sizeof(visor_event_header);
    uint16_t avail = static_cast<uint16_t>(out_cap - sizeof(visor_event_header));

    switch (ev.type_) {
    case EventType::MouseMove:
    case EventType::MouseDown:
    case EventType::MouseUp: {
        if (avail < sizeof(visor_pointer_payload)) {
            return false;
        }
        out->type = VISOR_EVENT_POINTER;
        visor_pointer_payload p;
        p.kind    = (ev.type_ == EventType::MouseDown) ? VISOR_POINTER_KIND_DOWN
                    : (ev.type_ == EventType::MouseUp) ? VISOR_POINTER_KIND_UP
                                                       : VISOR_POINTER_KIND_MOVE;
        p.x       = ev.mouse.x;
        p.y       = ev.mouse.y;
        p.dx      = ev.mouse.dx;
        p.dy      = ev.mouse.dy;
        p.buttons = ev.mouse.buttons;
        memcpy(tail, &p, sizeof(p));
        out->payload_len = static_cast<uint16_t>(sizeof(p));
        break;
    }
    case EventType::KeyDown:
    case EventType::KeyUp: {
        if (avail < sizeof(visor_keycode_payload)) {
            return false;
        }
        out->type  = VISOR_EVENT_KEYCODE;
        /* Derive press/release from the dispatch type_ (not ev.key.pressed) so the
         * switch that accepted this event and the PRESSED flag the deserialiser
         * reads are authoritative from one source -- the two can never diverge
         * even if a producer ever lets type_ and key.pressed disagree. */
        out->flags = (ev.type_ == EventType::KeyDown) ? VISOR_EVENT_FLAG_PRESSED : 0;
        visor_keycode_payload k;
        k.ascii     = ev.key.ascii;
        k.scancode  = ev.key.scancode;
        k.modifiers = static_cast<uint8_t>((ev.key.shift ? VISOR_KEYMOD_SHIFT : 0u) |
                                           (ev.key.ctrl ? VISOR_KEYMOD_CTRL : 0u) |
                                           (ev.key.alt ? VISOR_KEYMOD_ALT : 0u));
        memcpy(tail, &k, sizeof(k));
        out->payload_len = static_cast<uint16_t>(sizeof(k));
        break;
    }
    default:
        /* An EventType with no serialiser case is dropped here. Adding a new
         * EventType in event.hpp requires updating BOTH this serialiser and
         * visor_event_to_cinux's deserialiser switch. */
        return false;
    }
    return true;
}

/* ============================================================
 * L2 Time: PIT uptime in ms (truncated to uint32_t -- ~49 day wrap).
 * ============================================================ */
uint32_t cinux_now_ms(void* ctx) {
    (void)ctx;
    return static_cast<uint32_t>(cinux::drivers::PIT::get_uptime_ms());
}

/* ============================================================
 * Memory / log.
 * ============================================================ */
void* cinux_alloc(void* ctx, size_t n) {
    (void)ctx;
    return cinux::mm::kmalloc(n);
}

void cinux_free(void* ctx, void* p) {
    (void)ctx;
    cinux::mm::kfree(p);
}

__attribute__((format(printf, 2, 3))) void cinux_log(void* ctx, const char* fmt, ...) {
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    cinux::lib::kvprintf(fmt, ap);
    va_end(ap);
}

/* ============================================================
 * L1 Display: flush a dirty rect from the staging buffer to the framebuffer.
 *
 * §4c: the visor pump renders into the screen canvas's back buffer (the
 * staging Surface) and pushes only the dirty rects through this callback.
 * @p pixels is the staging buffer base; the rect at display coords (x,y,w,h)
 * lives at row offset y*stride + x*4 within it. We copy each row into the VBE
 * framebuffer (volatile MMIO), respecting its own pitch (VBE line alignment
 * may exceed width*4). This replaces the old Canvas::flip() full-frame copy --
 * identical pixels, but only the dirty rects are transferred and the display
 * path now runs through the Host ABI (host-agnostic).
 * ============================================================ */
void cinux_flush(void* ctx, int x, int y, int w, int h, const void* pixels, uint32_t stride,
                 visor_pixel_format fmt) {
    (void)ctx;
    if (g_fb == nullptr || pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    if (fmt != VISOR_PIX_XRGB8888) {
        return; /* Desktop framebuffer is 32bpp XRGB; other formats arrive later */
    }

    const uint32_t fb_pitch = g_fb->pitch();
    const uint32_t fb_w     = g_fb->width();
    const uint32_t fb_h     = g_fb->height();
    /* Clamp the rect to the framebuffer: a rect larger than the screen is
     * ill-formed, and this also bounds w before the *4u below so a pathological
     * caller (the flush is the single hard host seam) cannot wrap bytes_per_row. */
    if (w > static_cast<int>(fb_w)) {
        w = static_cast<int>(fb_w);
    }
    if (h > static_cast<int>(fb_h)) {
        h = static_cast<int>(fb_h);
    }
    const uint8_t*    src           = static_cast<const uint8_t*>(pixels);
    volatile uint8_t* dst           = reinterpret_cast<volatile uint8_t*>(g_fb->data());
    const uint32_t    bytes_per_row = static_cast<uint32_t>(w) * 4u;

    for (int row = 0; row < h; row++) {
        const int py = y + row;
        /* Dirty rects are clipped to screen bounds by WindowManager::invalidate,
         * but defend against a misbehaving caller regardless. */
        if (py < 0 || static_cast<uint32_t>(py) >= fb_h || x < 0 ||
            static_cast<uint32_t>(x) >= fb_w) {
            continue;
        }
        const uint32_t cols = (static_cast<uint32_t>(x) + bytes_per_row / 4u <= fb_w)
                                  ? bytes_per_row
                                  : (fb_w - static_cast<uint32_t>(x)) * 4u;
        const uint8_t* srow = src + static_cast<size_t>(py) * stride + static_cast<size_t>(x) * 4u;
        volatile uint8_t* drow =
            dst + static_cast<size_t>(py) * fb_pitch + static_cast<size_t>(x) * 4u;
        for (uint32_t b = 0; b < cols; b++) {
            drow[b] = srow[b];
        }
    }
}

/* ============================================================
 * Desktop extension: spawn.
 *
 * §3b: forwards to the in-tree shell-terminal helper. path / argv / fd are
 * accepted for ABI completeness but ignored -- a generic spawn(path, argv)
 * returning real stdio handles is §4+ work. Today there is exactly one
 * desktop action (open a shell), and create_shell_terminal() does exactly
 * that, so behaviour matches the non-visor gui_pump() path.
 * ============================================================ */
int cinux_spawn(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd) {
    (void)ctx;
    (void)path;
    (void)argv;
    if (stdin_fd != nullptr) {
        *stdin_fd = -1;
    }
    if (stdout_fd != nullptr) {
        *stdout_fd = -1;
    }
    create_shell_terminal();
    return 0;
}

}  // namespace

visor_host& cinux_visor_host() {
    return g_cinux_host;
}

void cinux_visor_host_init(cinux::drivers::Framebuffer* fb) {
    g_fb                               = fb;
    g_cinux_host.core.poll_event       = cinux_poll_event;
    g_cinux_host.core.flush            = cinux_flush;
    g_cinux_host.core.flush_complete   = nullptr; /* Desktop uses sync flush */
    g_cinux_host.core.enter_sleep      = nullptr; /* MCU-only (display off) */
    g_cinux_host.core.exit_sleep       = nullptr;
    g_cinux_host.core.now_ms           = cinux_now_ms;
    g_cinux_host.core.next_deadline_ms = nullptr; /* MCU __WFI throttling only */
    g_cinux_host.core.alloc            = cinux_alloc;
    g_cinux_host.core.free             = cinux_free;
    g_cinux_host.core.log              = cinux_log;

    g_cinux_desktop.spawn = cinux_spawn;
    g_cinux_host.desktop  = &g_cinux_desktop;
    g_cinux_host.ctx      = nullptr;

    cinux::lib::kprintf("[visor] Cinux host ABI adapter initialised\n");
}

}  // namespace cinux::gui
