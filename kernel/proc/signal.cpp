/**
 * @file kernel/proc/signal.cpp
 * @brief Signal disposition lookup, task registry, and delivery (F3-M1)
 *
 * Batch 1: default-disposition table + uncatchable predicate.
 * Batch 2: pid->Task registry, signal_send, deliverable selection, default
 *          action execution, and the top-level check-and-deliver entry.
 *
 * Namespace: cinux::proc
 */

#include "kernel/proc/signal.hpp"

#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

namespace {

// ============================================================
// Task registry: a simple singly-linked list keyed by pid, so sys_kill can
// resolve a target Task*.  Tasks register in Scheduler::add_task and leave
// in remove_task / exit_current.
// ============================================================

Task* g_registry_head = nullptr;

}  // namespace

void signal_register_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    task->registry_next = g_registry_head;
    g_registry_head     = task;
}

void signal_unregister_task(Task* task) {
    if (task == nullptr) {
        return;
    }
    Task** pp = &g_registry_head;
    while (*pp != nullptr) {
        if (*pp == task) {
            *pp                 = task->registry_next;
            task->registry_next = nullptr;
            return;
        }
        pp = &(*pp)->registry_next;
    }
}

Task* signal_find_task_by_pid(int pid) {
    for (Task* t = g_registry_head; t != nullptr; t = t->registry_next) {
        if (t->pid == pid) {
            return t;
        }
    }
    return nullptr;
}

// ============================================================
// Default disposition table (batch 1)
// ============================================================

// POSIX default dispositions.  Unlisted signals default to Terminate.
SigDefault signal_default_action(Signal sig) {
    switch (sig) {
    case Signal::kSigquit:
    case Signal::kSigill:
    case Signal::kSigtrap:
    case Signal::kSigabrt:
    case Signal::kSigbus:
    case Signal::kSigfpe:
    case Signal::kSigsegv:
        return SigDefault::kCoreDump;
    case Signal::kSigchld:
        return SigDefault::kIgnore;
    case Signal::kSigcont:
        return SigDefault::kContinue;
    case Signal::kSigstop:
    case Signal::kSigtstp:
    case Signal::kSigttin:
    case Signal::kSigtou:
        return SigDefault::kStop;
    default:
        return SigDefault::kTerminate;
    }
}

bool signal_is_uncatchable(Signal sig) {
    return sig == Signal::kSigkill || sig == Signal::kSigstop;
}

// ============================================================
// Delivery (batch 2)
// ============================================================

int signal_send(Task* target, Signal sig) {
    if (!signal_valid(static_cast<int>(sig))) {
        return -22;  // EINVAL
    }
    if (target == nullptr || target->state == TaskState::Zombie ||
        target->state == TaskState::Dead) {
        return -3;  // ESRCH
    }
    // A signal with disposition SIG_IGN is discarded unless it is uncatchable
    // (SIGKILL/SIGSTOP), which override SIG_IGN.
    if (!signal_is_uncatchable(sig) &&
        target->sig_actions[static_cast<int>(sig)].type == HandlerType::kIgnore) {
        return 0;
    }
    sig_set_add(target->sig_pending, sig);
    // TODO(batch later): wake a Blocked target for SIGKILL/SIGCONT/SIGTERM.
    return 0;
}

int signal_pick_deliverable(Task* task) {
    if (task == nullptr) {
        return 0;
    }
    const SigSet avail = task->sig_pending & ~task->sig_blocked;
    for (int n = 1; n <= kSignalMax; n++) {
        if ((avail & (SigSet{1} << n)) == 0) {
            continue;
        }
        // Batch 2 delivers only Default/Ignore dispositions.  Custom handlers
        // need a signal frame (built in batch 3), so leave them pending.
        if (task->sig_actions[n].type == HandlerType::kCustom) {
            continue;
        }
        task->sig_pending &= ~(SigSet{1} << n);
        return n;
    }
    return 0;
}

void signal_exec_default(Task* task, Signal sig) {
    switch (signal_default_action(sig)) {
    case SigDefault::kTerminate:
    case SigDefault::kCoreDump:
        task->exit_status = static_cast<int>(sig);
        cinux::lib::kprintf("[SIGNAL] default kill: tid=%u '%s' by SIG%d\n",
                            static_cast<unsigned>(task->tid), task->name, static_cast<int>(sig));
        Scheduler::exit_current();  // does not return
        break;
    case SigDefault::kIgnore:
        break;
    case SigDefault::kStop:
    case SigDefault::kContinue:
        // TODO(batch later): job-control stop/cont need a Stopped task state
        // and scheduler exclusion.  No-op for now.
        cinux::lib::kprintf("[SIGNAL] stop/cont not yet supported (SIG%d)\n",
                            static_cast<int>(sig));
        break;
    }
}

void signal_check_and_deliver() {
    Task* task = Scheduler::current();
    if (task == nullptr) {
        return;
    }
    int n = signal_pick_deliverable(task);
    if (n == 0) {
        return;
    }
    Signal           sig = static_cast<Signal>(n);
    const SigAction& act = task->sig_actions[n];
    switch (act.type) {
    case HandlerType::kDefault:
        signal_exec_default(task, sig);  // may not return (terminate)
        break;
    case HandlerType::kIgnore:
        break;
    case HandlerType::kCustom:
        // Batch 3: build a signal frame on the user stack and redirect RIP to
        // the user handler; sigreturn restores the saved context.
        break;
    }
}

}  // namespace cinux::proc
