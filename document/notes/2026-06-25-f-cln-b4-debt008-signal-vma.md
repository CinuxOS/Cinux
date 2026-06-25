# F-CLN 批4 — DEBT-008 signal_setup_frame 栈 VMA 校验 — 2026-06-25

> DEBT-008（signal_setup_frame 写帧不校验栈 VMA，深栈收信号二次 segfault）闭环。中风险（信号路径）。

## 问题
[signal_setup_frame](../../kernel/proc/signal.cpp#L301) 计算 `R = user_rsp - pad - 8 - sizeof(SignalFrame)` 后直接写 SignalFrame + trampoline，不校验 R 是否落合法 Stack VMA。深递归栈近底时收信号 → R 落 guard page 或 VMA 外 → 写帧中途 PF → 投 SIGSEGV，但此刻帧写一半、栈已损坏、原信号正投递 → 语义混乱偶现挂死。

## 修复
signal_setup_frame 计算 R 后、写帧前，加 VMA 校验：
- `Scheduler::current()->addr_space->vmas().find(R)` 须命中 VMA
- 该 VMA 须 `has_flag(Write)` + `has_flag(Stack)`
- 否则 `signal_exec_default(task, sig)` fallback（默认终止），不写帧
- IF=0 ISR 上下文（signal_check_deliver_isr 调），`vma_lock.irq_guard()` 是 no-op 锁（文档临界区）
- 踩坑：signal.cpp 在 `cinux::proc` ns，kprintf 须 `cinux::lib::kprintf` 限定（signal.cpp 之前没用裸 kprintf）

## 验证
- big_kernel 编译零 error/warning
- run-kernel-test **931/0**（kernel-mode ring0，signal_check_deliver_isr 严判 cs&3 只 user 投递，kernel-test 不覆盖 signal_setup_frame；校验不影响现有信号测试）
- make run 冒烟：启动到桌面（GUI worker launched + kernel_init exited + xHCI armed），**无 panic / 无 [SIGNAL] fallback 日志**（正常栈 VMA 合法，校验过）
- 边界（栈溢出收信号触发 fallback）靠逻辑正确性，留真用户态信号程序端到端 follow-up（同 F3-M4 STOP/CONT 范式）

## 关联
- GOTCHA#16（sigreturn trampoline 依赖 NXE 关闭，F9 后迁 vdso）+ #11（PF 硬门控 user-mode 判定）相关——本批只校验写帧前栈 VMA，trampoline/NXE 留 F9。
- 诚实记录：校验是防御性，正常路径不触发，边界场景无自动化覆盖（user-mode 信号 + 深栈），靠代码正确性 + 实机。

## 产出
- signal.cpp（include mm/address_space+vma + 校验逻辑 + cinux::lib::kprintf）+ debt.md DEBT-008 ✅ + PLAN 批4 ✅

下个：批5 DEBT-009 clear_user_mappings 识 huge entry。
