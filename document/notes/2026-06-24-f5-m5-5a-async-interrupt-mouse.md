# F5-M5 批5A — HID boot mouse 生产闭环（async interrupt-IN）

> 日期 2026-06-24 / 批 5A / 分支 feat/f5-m5-xhci-3 / commit e043d89

## 背景

批 0A–4B 已建成 xHCI 全栈（MSI-X / 控制器 / ring / 枚举 / HID boot mouse 配置 + 注入分层），但**生产路径从未接通**：`main.cpp` 无 USB 接线；`make run` 用 PS/2 tablet（`-device usb-tablet` 抢占宿主指针 → PS/2 收不到 → GUI 光标不动）。更关键的是**模型错误**：interrupt-IN 报告走同步 `XhciSlot::poll_interrupt_in`，内部 `run_transfer` **跑满 100 万次忙等**，静止鼠标 NAK 超时才返回——这是 test kernel 无 `sti` 的权宜，搬到生产 worker 会"静止卡死忙等、无法 yield"。

USB HID interrupt-IN 的**正确模型是周期异步**：提交 transfer 后挂起，静止 NAK（host 软件无感、零 CPU），设备有数据时 controller 完成 transfer → Transfer Event → MSI-X 中断（批2C 已武装 vector 0x40）→ 解码 + 注入 + re-arm。批5A 把模型改对，接通 mouse。

## 设计：async + TransferListener 分发

**TransferListener 抽象**（[xhci_controller.hpp](../../kernel/drivers/usb/xhci_controller.hpp)）—— controller 通用传输层**不解释 payload**（避坑批4B 记的分层 antipattern：传输类混入 mouse 应用语义）。controller 持 `by_slot_[64]` 注册表，`poll_events()` 遇 Transfer Event 按 slot 分发给注册的 listener：

- `TransferListener::on_transfer_complete(slot, epid, ev)` —— 设备驱动实现
- `XHCIController::register_transfer_listener(slot, listener)` / `ring_doorbell(slot, epid)`
- `XhciSlot::submit_interrupt_in_async(hc, buf, len)` —— enqueue Normal TRB + doorbell，**不调 run_transfer**（与同步 `poll_interrupt_in` 共用 enqueue，区别不阻塞）

**UsbMouse 实现 TransferListener**：`on_transfer_complete`（hard-IRQ 上下文，与 PS/2 `irq12_handler` 对称）→ `decode_boot_mouse` → `Mouse::inject_usb_motion`（dy 不取反，HID 约定）→ `arm()`（re-arm）。`arm()` 提交首次/后续 async transfer。

**boot 接线**（NEW [usb_init.cpp](../../kernel/drivers/usb/usb_init.cpp)，提到独立文件免 main.cpp 超 500 行）：`usb::init()` 扫端口 → 枚举（照 test_hid_mouse 序列）→ `find_boot_mouse` → `UsbMouse::init` → `register listener` → `arm()`（首次）→ `Mouse::set_usb_primary(true)`。graceful：`find_xhci` 失败则跳过（基线 run-kernel-test 无 qemu-xhci 不破）。[main.cpp](../../kernel/main.cpp) Step 21b 调用（sti 后、scheduler 前；中断驱动无需 worker/scheduler）。

## 关键 GOTCHA

1. **同步 poll 100 万次 nested-KVM 超时**：`test_hid_mouse` 的 `mouse.poll()` 对静止鼠标 NAK 跑满 `kResetIters`（100 万）MMIO 忙等，nested-KVM 下每次 VM-exit 慢，整体 >40s。该 poll 是 best-effort 无断言，且生产已改 async——**移除它**（保留 init 断言），test 快速绿。
2. **init 不 arm**：初版把 `arm()` 放进 `UsbMouse::init()`，但 test 也调 init → test 里多了个 pending async transfer，让 QEMU 持续 service 该 EP，拖慢同步 poll。正确分层：**init 不 arm**（test 同步 poll 不受影响），production 显式调 `arm()`。
3. **listener 注册 vs ISR 读并发**：register 在首次 async submit 之前（boot 序保证），submit 前无 transfer event 可能 → 无 race 窗口，免锁。
4. **re-arm 不递归**：re-arm 的 doorbell 不在当前 ISR 产生新 event（controller 下个 service interval 才查设备）→ poll_events 处理完当前批次返回。

## QEMU 切换

`make run` 移除 `-device usb-tablet`（tablet 抢占宿主指针），改挂 `-device qemu-xhci + -device usb-mouse`。usb-tablet 是 USB 设备、guest 从未驱动（4A/4B 是 xHCI mouse，非 UHCI tablet），PS/2 走 QEMU 默认 i8042——故从 COMMON_FLAGS 移除 tablet 对基线无害。PS/2 驱动代码保留（`set_usb_primary` 禁其喂队列，作静默 fallback）。

## 验证

| 目标 | 结果 |
|------|------|
| run-kernel-test（基线，无 xhci） | 931/0（usb::init graceful skip） |
| run-kernel-test-xhci（qemu-xhci+usb-mouse） | 931/0 ALL PASSED |
| make run（big_kernel，production async） | `[xHCI] HID boot mouse armed slot=1 ep1-IN` + GUI 桌面 + 无 panic |
| test_host | 绿（xhci 等 host 测） |
| 全量编译 | 过（含 test/unit，CI 盲区） |

**模型正确性**：make run boot 后 mouse armed，静止 40s 无中断风暴迹象（不卡不崩）—— async transfer pending，静止 NAK 零 CPU。鼠标实际移动需手动（QEMU VNC 移宿主鼠标看光标跟随，留用户验证）。

> 注：run-kernel-test-xhci 在本机（nested-KVM + xHCI 设备模拟）需 ~50s 跑完，超 timeout 40；用 timeout 60 验证。

## 下批

**5B**：USB boot keyboard —— 复用 5A 的 async + TransferListener 机制。`hid.hpp` 加 `decode_boot_keyboard` + `find_boot_keyboard`；`Keyboard` 加 `inject_usb_report`（边沿检测 press/release + HID keycode→ASCII 表）+ keyboard primary；NEW `UsbKeyboard`；boot 接线枚举 keyboard；`make run` 加 usb-kbd。**5C**：测试加固 + ROADMAP/PLAN/todo/notes 同步。
