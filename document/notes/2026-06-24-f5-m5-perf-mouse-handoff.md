# F5-M5 启动慢修复 + 鼠标不动排查交接 — 2026-06-24

> 分支 `feat/f5-m5-xhci-3`。**未 commit**（等用户反馈）。启动慢已修复（待 commit），鼠标不动未解决（交接新 AI 排查）。
>
> 这篇是交接笔记：记录已查清的根因/修复 + 鼠标不动的诊断状态 + 下一步具体方向，让接手的 AI 快速上手。

## 1. 背景

用户 make run 反馈两个问题：
1. **启动慢**：开机后过几秒才进桌面（不是立即）。
2. **鼠标不动**：进桌面后移动宿主鼠标，GUI 光标不跟随。

用户要求"找出同步操作异步化"。本笔记作者诊断出启动慢根因并修复（未 commit），鼠标不动未解决。

## 2. 启动慢（已修复，未 commit）

### 根因（TSC 实测定位）

用 `rdtsc` 在 boot 各阶段打时间戳，测出：

| 阶段 | 耗时（KVM TSC，/3GHz≈秒） |
|------|------|
| boot 早期 Step 1-17（GDT/IDT/PMM/VMM/slab/.../sti） | 116ms（快） |
| Step 17b-22（SMP/PCI/AHCI/Scheduler） | 60ms（快） |
| **kernel_init → gui_worker（ext2/VFS/gui/usb::init）** | **≈9 秒**（慢！） |
| 其中 **usb::init** | **≈8.6 秒** |

**usb::init 8.6 秒**的成因：它的同步 busy-poll（`XHCIController::init` reset/halt、`start` running、`port_reset`、`run_command`、`run_transfer` 各 100 万次 MMIO 轮询，常量 `kResetIters=1000000`）在 **WSL2 nested-KVM** 下每次 MMIO 读 = 一次 VM-exit ≈ μs，100 万 × 多循环累积 8 秒。

### 关键副发现：PIT 中断在 vCPU busy 期间几乎停投递

诊断时发现 boot 全程 `Scheduler::tick()`（PIT IRQ0 → handler）**只被调到第 ~5-8 次**（PIT `tick_count`），而 kernel_init 跑了 9 秒（本应几百 tick）。即 **PIT 中断在 vCPU 持续 busy（usb::init 的 100 万次 MMIO poll）期间，KVM 的中断注入率暴跌**——vCPU 没有中断窗口/HLT 让 KVM 注入。后果：系统从抢占式（PIT tick → schedule）退化成**协作式**（只有 yield/block 才切任务）。所以 kernel_init 不 yield（busy poll）就独占 CPU，gui_worker 要等 kernel_init exit（9 秒后）才第一次跑 → 桌面延迟。

> 这个现象 IF=1（kernel_init 全程 `pushfq` 读 IF=1），不是关中断，是 KVM 在 busy vCPU 上不注入周期中断的特性。**可能也影响 USB 中断（MSI-X）**——见第 4 节鼠标排查。

### 修复（两处，均在 [xhci_controller.cpp](../../kernel/drivers/usb/xhci_controller.cpp)）

1. **所有 busy-poll 循环加 `Scheduler::yield()`（每 10000 次）**：`init` halt/reset、`start` running、`port_reset`、`run_command`、`run_transfer`。yield → context_switch → vCPU 切换 → KVM 恢复中断注入 → gui_worker 交替渲染。需 `#include "kernel/proc/scheduler.hpp"`。
2. **`kResetIters` 100万 → 10万**：命令/ reset 完成通常**几次 poll 就 break**（实测 QEMU 很快），跑满 100 万纯是超时上限的浪费，nested-KVM MMIO 慢放大了这个浪费。10 万仍足够（命令成功 break 远不到 10 万）。

### 验证 + 量化

- run-kernel-test 931/0、run-kernel-test-xhci 931/0（kResetIters 10万不破 test_xhci，命令仍成功 break）。
- make run：gui_worker（`[GUI] Worker thread started`）现在出现在 `[xHCI] armed` **之前**（之前在之后，证明桌面早渲染了）。
- 量化（TSC）：桌面（gui_worker 首跑）**9 秒 → 1.6 秒**；usb::init **8.6 秒 → 0.94 秒**；mouse armed 0.94 秒（比桌面还早）。
- 剩余桌面 1.6 秒大头：`gui_start`（cgui 桌面 composite）≈0.76 秒——F13 cgui 渲染问题，非 usb。

## 3. 鼠标不动（未解决 — 交接排查）

### 路径回顾

USB mouse（async interrupt-IN，批5A/5B）：
```
宿主鼠标 → QEMU usb-mouse 设备 → xHCI: mouse 动 → controller 完成 interrupt-IN transfer
→ Transfer Event 落 event ring + IMAN.IP 置位
→ MSI-X 中断（vector 0x40，批2C 武装）→ CPU → xhci_irq_handler → event_irq_thunk
→ poll_events() → 按 slot 查 by_slot_[] → UsbMouse::on_transfer_complete
→ decode_boot_mouse → Mouse::inject_usb_motion → apply_motion（更新 mouse_x_/y_ + enqueue GUI MouseEvent）
→ gui_worker pump 消费 event_queue → 光标重绘
```

### 已确认 / 已排除

- ✅ mouse armed 成功（`[xHCI] HID boot mouse armed: port=5 ep1-IN`，0.94 秒）。
- ✅ `set_usb_primary(true)` 设了（PS/2 不喂队列，USB primary）。
- ✅ 批2C MSI-X 武装 + 命令管线中断验证过（NOOP → Command Completion Event）。**但 transfer 完成（mouse report）的中断未单独验证过**——批2C 只证了命令完成，没证 transfer 完成。
- ❓ **PIT 停的副发现可能也影响 MSI-X**：vCPU busy 期间 KVM 不注入周期中断。gui_worker 跑后 vCPU 是否还持续 busy（pump 渲染无 yield？或别的）？MSI-X（edge，vector 0x40）是否也被 KVM 在 busy vCPU 上不注入？**这是首要怀疑点**。

### 下一步诊断方向（按顺序，给接手 AI）

**A. 确认 USB transfer 完成中断是否到 CPU**（首要）：
- 在 `UsbMouse::on_transfer_complete`（[usb_mouse.cpp](../../kernel/drivers/mouse/usb_mouse.cpp)）开头加 `kprintf("[MOUSE-IRQ] report!\n")`，make run 移鼠标，看 log 是否出现。
- 或读 `g_xhci_irq_count`（[xhci_irq.cpp](../../kernel/drivers/usb/xhci_irq.cpp)）——MSI-X handler 每次自增。mouse 动后应增长。在 gui_worker 里周期打印它。
- **如果 on_transfer_complete 没触发 / irq_count 不增长** → MSI-X 中断没到 CPU → 查 MSI-X 武装（[msix_controller.cpp](../../kernel/drivers/pci/msix_controller.cpp)）+ vector 0x40 注册（[irq_handlers.cpp](../../kernel/arch/x86_64/irq_handlers.cpp) `xhci_irq_stub`）+ 是否被 KVM 在 busy vCPU 上不注入（参考第 2 节 PIT 现象）。

**B. 如果中断到了，查分发**：
- `poll_events`（[xhci_controller.cpp](../../kernel/drivers/usb/xhci_controller.cpp)）遇 Transfer Event 时按 `cmd_completion_slot_id(ev.control)` 查 `by_slot_[slot]`。确认 mouse slot_id 注册对（`register_transfer_listener(slot_id, &g_mouse)`，[usb_init.cpp](../../kernel/drivers/usb/usb_init.cpp)）。加 log 看 Transfer Event 的 slot/epid 是否匹配 mouse。

**C. 如果 on_transfer_complete 触发了，查 inject**：
- `Mouse::inject_usb_motion` → `apply_motion`（[mouse.cpp](../../kernel/drivers/mouse/mouse.cpp)）更新 `mouse_x_/y_` + `g_event_queue_.enqueue`。加 log 看 dx/dy/buttons 是否非零（mouse 动有 report）。

**D. 如果 inject 了，查 GUI 消费**：
- `Mouse::apply_motion` enqueue 了 `MouseMove` 事件。gui_worker pump（[init.cpp gui_worker_thread](../../kernel/proc/init.cpp) `cinux::gui::pump`）是否 dequeue + 移光标？查 cgui host adapter（[host_cinux.cpp](../../kernel/gui/host_cinux.cpp)）的 poll_event / 光标绘制。**注意**：光标渲染源是 `Mouse::x()/y()` 还是 event_queue？如果 GUI 直接读 `Mouse::x()/y()`（apply_motion 更新了），光标应动；如果 GUI 读 event_queue 的 MouseEvent.dx/dy，需 pump 消费。

**E. 环境**：QEMU `-vnc :0`（[qemu.cmake make run](../../cmake/qemu.cmake)）。用户是否正确把宿主鼠标捕获进 VNC（usb-mouse 需宿主输入）？`-device usb-mouse` 是否真收到宿主移动？可在 QEMU 加 `-device usb-mouse` 观察，或换 `usb-tablet` 对照（但 tablet 走 UHCI，guest xHCI 驱动看不到——仅作"宿主鼠标是否到 QEMU"的对照）。

### 给接手 AI 的提示

- 别重复第 2 节的启动慢诊断（已修）。聚焦鼠标中断链路 A→E。
- **PIT 停现象是关键线索**：KVM 在 busy vCPU 上不注入周期中断。MSI-X（edge）可能同病。yield 修复让 vCPU 不持续 busy，但 gui_worker pump 是否又让 vCPU busy（渲染）？查 gui_worker 是否在 pump 内 yield（当前是 pump 后 yield）。
- usb::init 的 async 模型（批5A/5B）正确（on_transfer_complete decode+inject+re-arm），代码审过 0 bug（4-agent 对抗 review）。问题更可能在**中断投递**（A）或 **GUI 消费**（D）。

## 4. 未 commit 改动清单（本会话，待用户决定）

启动慢修复（鼠标排查未改代码）：
- [xhci_controller.cpp](../../kernel/drivers/usb/xhci_controller.cpp)：`kResetIters` 100万→10万；`init`/`start`/`port_reset`/`run_command`/`run_transfer` 的 busy 循环加 `Scheduler::yield()`（每 10000）；`#include scheduler.hpp`。
- 诊断 log 已全部清理（scheduler [SCHd]/[Tk]、pit tick log、usb_init IF/cycles/tsc、init TSC、main TSC）——当前工作树干净（只含上述修复）。

> 注：7791352 之前已 commit（usb::init 移 kernel_init + USB primary 日志）。本会话的 yield/kResetIters 修复在其上，**未 commit**。

## 5. 环境

- WSL2 nested-KVM（/dev/kvm 在）。make run 用 `-accel kvm`。
- QEMU `-vnc :0`（headless，用户 VNC 看）。
- `run-kernel-test-xhci` 在本机需 ~50 秒（nested-KVM + xHCI 设备模拟），超 timeout 40，用 timeout 60。
