# F-CLN 批0 — xHCI/USB 子系统专项审 — 2026-06-25

> F-CLN 债务清理里程碑批0。补审 Q3（2026-06-21）后合入的 F5-M5 xHCI 未审面。**只读，零代码改动**。

## 范围
`kernel/drivers/{xhci,usb}/` + `mouse/{usb_mouse,usb_tablet,hid}.*` + `keyboard/usb_keyboard.*`。
维度 D2 内存 + D3 并发 + D4 生命周期。deterministic 四段式（A 锚点 / B 不变点 / C 门槛 / D 闭环）。

## 结论
- **D2/D4 清洁**：零堆分配（`grep 'new\|delete\|kmalloc\|kfree'` 0 命中）——对象全全局静态（`g_xhci`/`g_slots[2]`/`g_tablet`/`g_keyboard`）+ DmaBuffer move-only RAII 值成员。F1-M3 DmaBuffer + F5-M5 批4B/5A 分层重构质量高，无 UAF/leak 风险面。
- **D3 fail → DEBT-021（P1）**：`XHCIController::poll_events`（[xhci_controller.cpp:272-302](../../kernel/drivers/usb/xhci_controller.cpp#L272-L302)）无锁，三上下文调：
  ① gui_worker 线程每帧（[init.cpp:53](../../kernel/proc/init.cpp#L53)）
  ② usb::init 枚举线程 run_command/run_transfer busy-poll（:311/:331）
  ③ MSI-X ISR event_irq_thunk（:389，QEMU+nested-KVM 下潜伏）
  `EventRing::dequeue`（[xhci_ring.cpp:61](../../kernel/drivers/usb/xhci_ring.cpp#L61)）非原子。**①② 当前就并发**（kernel_init_thread 跑 usb::init 时 gui_worker 已 launch 持续 pump）。侥幸不炸（枚举快 0.94s + event ring 多空）。真机 MSI-X 触发后更剧。

## 产出
- 报告：`document/todo/quality/reports/2026-06-25-xhci-usb-audit.md`
- 新债：**DEBT-021** 登记到 `debt.md` 🟠 High（P1）
- 修复建议：poll_events 加 irq-safe Spinlock（覆盖 dequeue + ERDP + listener 分发），对齐 Linux xHCI `spin_lock_irqsave` 单上下文化。

## 关联确认（非重复债）
- xHCI/HID 分层坏味道已闭环（`8ff216f` + `dbe8f14`），本次确认 TransferListener 机制守住分层边界 → pass。
- 鼠标 SPSC 互斥（usb_primary_ 让 PS/2 闭嘴）批4B 已做 → pass（PS/2 vs USB 维度；DEBT-021 是 USB 内部多上下文，不同层）。

下个：批1 DEBT-015 核实关债（PathBuf 已改堆，预期很小）。
