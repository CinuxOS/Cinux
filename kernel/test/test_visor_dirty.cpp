/**
 * @file kernel/test/test_visor_dirty.cpp
 * @brief QEMU in-kernel tests for the visor dirty-region + flush path (F13 §4c)
 *
 * Two layers:
 *   1. WindowManager dirty-mechanism unit tests -- invalidate/invalidate_all/
 *      clear_dirty/clipping directly on the Region (no pump, no Mouse globals).
 *   2. visor_pump integration tests -- a fake visor_host whose flush callback
 *      records each (x,y,w,h) rect; drives the real pump and asserts the
 *      dirty-gated composite+flush behaviour: first frame flushes the whole
 *      screen, an idle pump flushes nothing, an invalidated rect is forwarded.
 *
 * Compile condition: CINUX_GUI.
 */

#include <stdint.h>

#include "big_kernel_test.h"

#ifdef CINUX_GUI
#    include "kernel/drivers/canvas.hpp"
#    include "kernel/gui/visor_core/visor_host.h"
#    include "kernel/gui/visor_core/visor_pump.hpp"
#    include "kernel/gui/visor_core/visor_region.hpp"
#    include "kernel/gui/window_manager.hpp"
#endif

#ifdef CINUX_GUI

using visor::Rect;
using visor::Region;
using cinux::drivers::Canvas;
using cinux::gui::WindowManager;

namespace {

/// A recorded flush rect (what the fake host received).
struct FlushRect {
    int x;
    int y;
    int w;
    int h;
};

FlushRect g_flushed[64];
uint32_t  g_flushed_n = 0;

/// Fake host flush: ignore the pixels, just record the rect the pump pushed.
void record_flush(void* /*ctx*/, int x, int y, int w, int h, const void* /*pixels*/,
                  uint32_t /*stride*/, visor_pixel_format /*fmt*/) {
    if (g_flushed_n < 64) {
        g_flushed[g_flushed_n] = FlushRect{x, y, w, h};
        g_flushed_n++;
    }
}

Canvas g_screen;  ///< Shared screen canvas for the singleton pump tests

/// Build a host whose only wired callback is the recording flush.
visor_host make_recording_host() {
    visor_host h{};
    h.core.flush = record_flush;
    h.ctx        = nullptr;
    h.desktop    = nullptr;
    return h;
}

/// Reset the singleton WM + shared canvas for a clean pump test.
void setup_pump_wm(uint32_t w, uint32_t h) {
    g_screen.init(w, h);
    auto& wm = WindowManager::instance();
    wm.init(&g_screen, nullptr);
    wm.clear_dirty();
    wm.reset_cursor_tracking();
    g_flushed_n = 0;
}

}  // namespace

// ============================================================
// WindowManager dirty-mechanism unit tests (no pump)
// ============================================================

namespace test_visor_dirty_api {

void test_invalidate_adds_clipped_rect() {
    Canvas screen;
    screen.init(100, 100);
    WindowManager wm;
    wm.init(&screen, nullptr);

    wm.invalidate(Rect{10, 10, 20, 20});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    TEST_ASSERT_EQ(wm.dirty().rects()[0].x0, 10);
    TEST_ASSERT_EQ(wm.dirty().rects()[0].y1, 20);

    /* Off-screen rect is clipped to empty and dropped. */
    wm.invalidate(Rect{200, 200, 300, 300});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);

    wm.clear_dirty();
    TEST_ASSERT_TRUE(wm.dirty().empty());
}

void test_invalidate_all_covers_screen() {
    Canvas screen;
    screen.init(80, 60);
    WindowManager wm;
    wm.init(&screen, nullptr);

    wm.invalidate_all();
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    Rect b = wm.dirty().bounds();
    TEST_ASSERT_EQ(b.x0, 0);
    TEST_ASSERT_EQ(b.y0, 0);
    TEST_ASSERT_EQ(b.x1, 80);
    TEST_ASSERT_EQ(b.y1, 60);
}

void test_invalidate_clips_partial_offscreen() {
    Canvas screen;
    screen.init(50, 50);
    WindowManager wm;
    wm.init(&screen, nullptr);

    /* Rect spills past the right/bottom edge -> clipped to the screen. */
    wm.invalidate(Rect{40, 40, 200, 200});
    TEST_ASSERT_EQ(wm.dirty().count(), 1u);
    Rect r = wm.dirty().rects()[0];
    TEST_ASSERT_EQ(r.x0, 40);
    TEST_ASSERT_EQ(r.y0, 40);
    TEST_ASSERT_EQ(r.x1, 50);
    TEST_ASSERT_EQ(r.y1, 50);
}

}  // namespace test_visor_dirty_api

// ============================================================
// visor_pump dirty-gated composite + flush integration tests
// ============================================================

namespace test_visor_pump_flush {

void test_first_frame_flushes_full_screen() {
    setup_pump_wm(80, 60);
    visor_host h = make_recording_host();
    cinux::gui::visor_pump(&h); /* sentinel cursor -> invalidate_all -> flush */

    TEST_ASSERT_EQ(g_flushed_n, 1u);
    /* The single rect must cover the whole 80x60 screen. */
    TEST_ASSERT_EQ(g_flushed[0].x, 0);
    TEST_ASSERT_EQ(g_flushed[0].y, 0);
    TEST_ASSERT_EQ(g_flushed[0].w, 80);
    TEST_ASSERT_EQ(g_flushed[0].h, 60);
}

void test_idle_pump_flushes_nothing() {
    setup_pump_wm(80, 60);
    visor_host h = make_recording_host();
    cinux::gui::visor_pump(&h); /* first frame */
    g_flushed_n = 0;

    cinux::gui::visor_pump(&h); /* idle: mouse still, no terminal output */
    TEST_ASSERT_EQ(g_flushed_n, 0u);
}

void test_invalidated_rect_is_flushed() {
    setup_pump_wm(80, 60);
    visor_host h = make_recording_host();
    cinux::gui::visor_pump(&h); /* first frame (cursor sentinel consumed) */
    g_flushed_n = 0;

    /* Mouse is still, so invalidate_cursor_move() is a no-op; the only dirty
     * rect is the one we add explicitly. */
    WindowManager::instance().invalidate(Rect{5, 5, 15, 15});
    cinux::gui::visor_pump(&h);

    TEST_ASSERT_EQ(g_flushed_n, 1u);
    TEST_ASSERT_EQ(g_flushed[0].x, 5);
    TEST_ASSERT_EQ(g_flushed[0].y, 5);
    TEST_ASSERT_EQ(g_flushed[0].w, 10);
    TEST_ASSERT_EQ(g_flushed[0].h, 10);
}

void test_dirty_cleared_after_flush() {
    setup_pump_wm(80, 60);
    visor_host h = make_recording_host();
    cinux::gui::visor_pump(&h); /* flushes + clears dirty */

    TEST_ASSERT_TRUE(WindowManager::instance().dirty().empty());
}

}  // namespace test_visor_pump_flush

extern "C" void run_visor_dirty_tests() {
    TEST_SECTION("visor Dirty/Flush Tests (F13 §4c)");
    RUN_TEST(test_visor_dirty_api::test_invalidate_adds_clipped_rect);
    RUN_TEST(test_visor_dirty_api::test_invalidate_all_covers_screen);
    RUN_TEST(test_visor_dirty_api::test_invalidate_clips_partial_offscreen);
    RUN_TEST(test_visor_pump_flush::test_first_frame_flushes_full_screen);
    RUN_TEST(test_visor_pump_flush::test_idle_pump_flushes_nothing);
    RUN_TEST(test_visor_pump_flush::test_invalidated_rect_is_flushed);
    RUN_TEST(test_visor_pump_flush::test_dirty_cleared_after_flush);
    TEST_SUMMARY();
}

#else /* !CINUX_GUI */

extern "C" void run_visor_dirty_tests() {}

#endif /* CINUX_GUI */
