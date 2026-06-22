/**
 * @file kernel/gui/visor_core/visor_pump.cpp
 * @brief visor core pump implementation -- table-driven GUI iteration
 *
 * See visor_pump.hpp for scope. This body reads input, spawns and renders
 * strictly through the visor_host table; it has no direct dependency on the
 * mouse driver, PIT, framebuffer or process layer. The Cinux adapter
 * (visor_host_cinux.cpp) fills that table; a different fill would run this
 * same body on a different host.
 *
 * Compile condition: CINUX_GUI.
 */

#include "visor_pump.hpp"

#include <stdint.h>

#include "kernel/gui/desktop_icon.hpp"  // IconAction
#include "kernel/gui/event.hpp"
#include "kernel/gui/gui_init.hpp"  // create_shell_terminal (fallback spawn)
#include "kernel/gui/terminal.hpp"
#include "kernel/gui/window_manager.hpp"
#include "kernel/lib/string.hpp"  // memcpy (freestanding, GOTCHA#9)
#include "visor_event.h"
#include "visor_event_payload.h"
#include "visor_region.hpp"  // visor::Region / Rect (dirty flush)

namespace cinux::gui {
namespace {

/* Largest payload we ever need to buffer behind a header (pointer = 18 B). */
constexpr uint16_t kMaxPayload = 24;

/**
 * @brief Deserialise a visor event (header + payload) into a cinux::gui::Event
 *
 * Validates magic/version and that the payload is large enough for its type,
 * then copies fields out via memcpy (the payload is packed, so typed access
 * could be unaligned on some hosts). Returns false on any mismatch, in which
 * case the caller drops the event and continues.
 *
 * @param hdr   the fixed-width header (already validated to carry magic/version)
 * @param tail  byte pointer to the payload immediately following @p hdr
 * @param out   receives the decoded event
 * @return      true if decoded successfully
 */
bool visor_event_to_cinux(const visor_event_header* hdr, const uint8_t* tail, Event& out) {
    if (hdr->magic != VISOR_EVENT_MAGIC || hdr->version != VISOR_ABI_VERSION) {
        return false;
    }

    switch (hdr->type) {
    case VISOR_EVENT_POINTER: {
        if (hdr->payload_len < sizeof(visor_pointer_payload)) {
            return false;
        }
        visor_pointer_payload p;
        memcpy(&p, tail, sizeof(p));
        switch (p.kind) {
        case VISOR_POINTER_KIND_MOVE:
            out.type_ = EventType::MouseMove;
            break;
        case VISOR_POINTER_KIND_DOWN:
            out.type_ = EventType::MouseDown;
            break;
        case VISOR_POINTER_KIND_UP:
            out.type_ = EventType::MouseUp;
            break;
        default:
            return false; /* corrupt kind -- reject, matching magic/version strictness */
        }
        out.mouse.x       = p.x;
        out.mouse.y       = p.y;
        out.mouse.dx      = p.dx;
        out.mouse.dy      = p.dy;
        out.mouse.buttons = p.buttons;
        out.mouse.left    = (p.buttons & 0x1u) != 0;
        out.mouse.right   = (p.buttons & 0x2u) != 0;
        out.mouse.middle  = (p.buttons & 0x4u) != 0;
        return true;
    }
    case VISOR_EVENT_KEYCODE: {
        if (hdr->payload_len < sizeof(visor_keycode_payload)) {
            return false;
        }
        visor_keycode_payload k;
        memcpy(&k, tail, sizeof(k));
        const bool pressed = (hdr->flags & VISOR_EVENT_FLAG_PRESSED) != 0;
        out.type_          = pressed ? EventType::KeyDown : EventType::KeyUp;
        out.key.ascii      = k.ascii;
        out.key.scancode   = k.scancode;
        out.key.pressed    = pressed;
        out.key.shift      = (k.modifiers & VISOR_KEYMOD_SHIFT) != 0;
        out.key.ctrl       = (k.modifiers & VISOR_KEYMOD_CTRL) != 0;
        out.key.alt        = (k.modifiers & VISOR_KEYMOD_ALT) != 0;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

void visor_pump(visor_host* host) {
    if (host == nullptr) {
        return;
    }
    auto& wm = WindowManager::instance();

    /* §3b: host time is read through the table (NULL-guarded) but not consumed
     * yet -- the worker already yields between iterations. §4 frame-rate
     * throttling reads it. Reading here keeps every host dereference inside the
     * pump behind a uniform NULL guard, instead of one unguarded call site in
     * the caller. */
    uint32_t now_ms = (host->core.now_ms != nullptr) ? host->core.now_ms(host->ctx) : 0u;
    (void)now_ms;

    /* 1. Drain input through the Host ABI table, deserialise, dispatch. */
    if (host->core.poll_event != nullptr) {
        /* Fixed on-stack buffer: header (8) + up to kMaxPayload (24) tail. The
         * host writes at most sizeof(buf); visor_event_to_cinux validates
         * payload_len as a lower bound and the copy never exceeds the fixed
         * tail, so a buggy/short host write cannot overrun here. */
        alignas(uint32_t) uint8_t buf[sizeof(visor_event_header) + kMaxPayload];
        auto*                     hdr  = reinterpret_cast<visor_event_header*>(buf);
        uint8_t*                  tail = buf + sizeof(visor_event_header);
        while (host->core.poll_event(host->ctx, hdr, sizeof(buf))) {
            Event ev;
            if (visor_event_to_cinux(hdr, tail, ev)) {
                switch (ev.type_) {
                case EventType::MouseMove:
                case EventType::MouseDown:
                case EventType::MouseUp:
                    wm.handle_mouse(ev);
                    break;
                case EventType::KeyDown:
                case EventType::KeyUp:
                    wm.handle_key(ev);
                    break;
                }
            }
        }
    }

    /* 2. Deferred desktop-icon action: route through the Desktop extension so
     *    the spawn path is ABI-driven; fall back to the in-tree helper when the
     *    host has no desktop extension (e.g. MCU profile). */
    IconAction action = wm.consume_pending_icon_action();
    if (action == IconAction::OpenShell) {
        if (host->desktop != nullptr && host->desktop->spawn != nullptr) {
            int in_fd  = -1;
            int out_fd = -1;
            host->desktop->spawn(host->ctx, "/bin/sh", nullptr, &in_fd, &out_fd);
        } else {
            create_shell_terminal();
        }
        /* A new window appeared -- structural change, re-flush the frame. */
        wm.invalidate_all();
    }

    /* 3. Poll terminal output. poll_output() signals whether the shell produced
     *    data this iteration; any output dirties the screen (conservatively the
     *    whole frame -- terminal redraws can span the window). The terminal's
     *    own canvas is rendered regardless so it is fresh when composite()
     *    blits it. */
    for (uint32_t i = 0; i < wm.window_count(); i++) {
        auto* win = wm.window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<Terminal*>(win);
            if (term->poll_output()) {
                wm.invalidate_all();
            }
            /* A pipe-less terminal mutates content via on_key() without going
             * through the stdout pipe, so poll_output() misses it; consume its
             * content-dirty flag so a direct key echo still reaches the screen. */
            if (term->consume_content_dirty()) {
                wm.invalidate_all();
            }
            term->render_to_canvas();
        }
    }

    /* 4. Cursor footprint: if the mouse moved since last frame, mark the old +
     *    new cursor rects dirty. No-op when still; full-screen on first frame. */
    wm.invalidate_cursor_move();

    /* 5. Render + flush only when something changed (dirty non-empty). Idle
     *    iterations skip both composite() and the flush -- a real win over the
     *    old per-frame full flip(). composite() repaints the whole staging back
     *    buffer (always fresh); the host forwards exactly the dirty rects, so
     *    over-coverage is a perf cost, never a stale-pixel bug. */
    if (wm.dirty().empty()) {
        return;
    }

    wm.composite();

    /* Push each dirty rect through the Host ABI flush. The Cinux adapter
     * forwards the rect from the staging back buffer to the framebuffer; the
     * pixels pointer is the buffer base and (x,y,w,h) locates the dirty rect
     * within it. See visor_host.h for the flush contract. */
    if (host->core.flush != nullptr) {
        auto* screen = wm.screen();
        if (screen != nullptr && screen->back_buffer() != nullptr) {
            uint32_t*            base   = screen->back_buffer();
            const uint32_t       stride = screen->pitch();
            const visor::Region& dirty  = wm.dirty();
            for (uint32_t i = 0; i < dirty.count(); i++) {
                const visor::Rect& r = dirty.rects()[i];
                host->core.flush(host->ctx, r.x0, r.y0, static_cast<int>(r.width()),
                                 static_cast<int>(r.height()), base, stride, VISOR_PIX_XRGB8888);
            }
        }
    }

    wm.clear_dirty();
}

}  // namespace cinux::gui
