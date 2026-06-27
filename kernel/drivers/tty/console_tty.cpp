/**
 * @file kernel/drivers/tty/console_tty.cpp
 * @brief System console TTY singleton wiring (F10-M3 batch 2)
 */

#include "kernel/drivers/tty/tty.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::drivers {

// Static storage: the TTY ctor fills the default termios, so the instance is
// usable before console_tty_init() (just without an echo sink).
static TTY g_console_tty;

TTY& console_tty() {
    return g_console_tty;
}

// Echo through kprintf so typed chars reach the same sink as stdout (serial +
// the registered Console).  kprintf/Console::putc are lock-free, so calling
// this from the keyboard IRQ context cannot deadlock; a rare re-entrant
// overlap only risks a transient cursor glitch (deferred-echo is a follow-up).
static void echo_via_kprintf(char c, void* /*ctx*/) {
    cinux::lib::kprintf("%c", c);
}

void console_tty_init() {
    Termios tm;
    make_default_termios(tm);
    tm.c_cc[kVerase] = 0x08;  // keyboard driver emits ^H for the Backspace key
    g_console_tty.set_termios(tm);
    g_console_tty.set_echo_sink(echo_via_kprintf, nullptr);
}

}  // namespace cinux::drivers
