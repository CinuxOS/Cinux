# xHCI/USB 子系统专项审计报告 — 2026-06-25（F-CLN 批0）

> Q3 全量审计（2026-06-21，14/14 维度）后，F5-M5 xHCI（PR#30 + PR#31）才合入 main，此为补审的未审面。
> 范围：`kernel/drivers/{xhci,usb}/` + `kernel/drivers/mouse/{usb_mouse,usb_tablet,hid}.*` + `kernel/drivers/keyboard/{usb_keyboard}.*`。
> 维度：D2 内存 + D3 并发 + D4 生命周期（与 xHCI 最相关三维度）。
> 方法：deterministic 四段式（A 锚点 / B 不变点 / C 门槛 / D 闭环）。**只读，零代码改动**。

## A 锚点（grep 命中，机器数）

| 锚点 | 命令 | 命中 |
|---|---|---|
| 堆分配 | `grep -rn 'new \|delete \|kmalloc\|kfree'` | **0**（仅注释匹配） |
| DmaBuffer | `grep -rn DmaBuffer` | controller 6（dcbaa/scratch×2/cmd_ring/event_ring/erst）+ slot 5（dev_ctx/in_ctx/ep0_ring/data/int_ring），全值成员 |
| 锁/原子 | `grep -rn 'Spinlock\|irq_guard\|__atomic\|Atomic'` | **0**（xHCI/USB 零锁零原子） |
| poll_events 调用 | `grep -rn 'poll_events(' kernel/` | init.cpp:53（gui_worker 线程）+ controller :259/:312/:331（start/run_command/run_transfer）+ :389（ISR thunk）+ test |

## B 不变点（逐条 pass/fail/n/a）

### D2 内存安全
- **无堆分配**：对象全全局静态（`g_xhci`/`g_slots[2]`/`g_tablet`/`g_keyboard`）+ DmaBuffer 值成员 → **pass**
- **DmaBuffer move-only RAII**（F1-M3，move 后旧句柄 release 回调置空，不 double-free）→ **pass**
- **on_transfer_complete decode buffer**（`data_buf_` DmaBuffer，定长，越界由 TRB 长度约束）→ **pass**

### D3 并发（SMP / 中断）
- **poll_events 串行化保护** → **FAIL**（无锁，三上下文调，见 DEBT-021）
- **EventRing::dequeue 原子性** → **FAIL**（xhci_ring.cpp:61 非原子：读 `dequeue_`/检查 cycle/`++dequeue_`/wrap flip `ccs_` 全无保护）
- **by_slot_ register vs read 无锁** → **warn**（`register_transfer_listener` 写 / `poll_events` 读，当前一次性注册时序侥幸；运行时换/清 listener 则竞争）
- **on_transfer_complete → inject_usb_* → g_event_queue_ SPSC 单生产者** → **warn**（生产者上下文 gui_worker 或 ISR，多上下文破 SPSC 单生产者假设；当前 usb_primary_ 下 PS/2 闭嘴，USB 是唯一生产者，但 USB 自身多上下文）

### D4 生命周期
- **全局静态对象无析构**（kernel 永远活）→ **pass**（非 leak）
- **listener 指针** 指向全局 `g_tablet`/`g_keyboard`，永有效 → **pass**（无悬垂）
- **DmaBuffer phys 不 release**（全局对象不析构）→ **pass**（kernel 生命周期内无所谓）

## C 门槛
- **D2/D4 清洁**：零堆分配 + 全局静态 RAII 是高质量设计（F1-M3 DmaBuffer + F5-M5 分层重构 4B 收口的成果），无 UAF/leak 风险面。
- **D3 1 条 P1 债**（DEBT-021）。

## D 闭环（新债登记）
- **DEBT-021**（P1 并发）：`XHCIController::poll_events` 无锁，三上下文调（gui_worker 线程 + usb::init 枚举线程 + MSI-X ISR 潜伏），`EventRing::dequeue` 非原子 → 数据竞争。详见 `debt.md`。

## 关联既有结论（非重复）
- xHCI/HID 分层坏味道（传输类混应用语义）—— F5-M5 批4B + 5A 已闭环（`8ff216f` TransferListener + `dbe8f14` CODING-TASTE §13），本次审确认分层边界由 TransferListener 机制守住（controller 不解释 payload，listener 解码）→ **pass**。
- 鼠标 SPSC 互斥（usb_primary_ 让 PS/2 闭嘴）—— F5-M5 批4B 已做，本次确认 `process_byte` 早退（mouse.cpp:203）→ **pass**（PS/2 vs USB 维度；DEBT-021 是 USB 内部多上下文，不同层）。
