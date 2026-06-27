# SMP fork CoW 竞态修复 + syscall RBP ABI 修复（F10-M2）

> 分支 `feat/f10-musl`。接 [[2026-06-27-shell-launch-smp-investigation]]。
> shell（user task）在 -smp 2 下 fork+execve 启动 musl 程序偶发 fork 炸弹/#DF。本文记录根因 + 修复 + 复现器实证。

## 根因 1（主因，已修）：fork/clone CoW 改写父页表后不刷 TLB

`copy_page_table_level`（[fork.cpp](../../kernel/proc/fork.cpp) L49-102，fork 与 clone 共用）把**父进程自己的在用 PTE** 从 writable 改写成 read-only+`FLAG_COW`（L97-98 `src_table[i].raw &= ~FLAG_WRITABLE; |= FLAG_COW`），但**全程不刷 TLB**。父 CPU 仍缓存旧的 WRITABLE TLB 项 → fork 返回用户态后父进程下次写该页 **TLB 命中（writable）→ 不 #PF → 直接写到已共享的物理页**，污染子进程稍后的 CoW 副本。

- **单 CPU 稳**：下一次 `context_switch` 因 `prev->addr_space != next->addr_space` 重载 cr3（[scheduler.cpp](../../kernel/proc/scheduler.cpp) `switch_addr_space` → `AddressSpace::activate` = `write_cr3`），父 TLB 被刷，再写时正确 CoW。
- **-smp 2 炸**：父进程在自己 CPU 上持续运行，地址空间不变，cr3 不重载，陈旧 writable TLB 一直在 → 写穿透。这正是 note 里「单 CPU 稳、SMP 偶发」的机制。
- fork 与 clone(无 CLONE_VM) **都**调 `copy_page_table_level`，都有此 bug；note 里「musl clone 稳 6/6」是采样假象 + CLONE_VM 线程路径（根本不调 CoW 拷贝）。

**6-agent workflow 对抗验证确认**：根因 = TLB 一致性（非 mapcount 原子性——mapcount 已是 `__atomic ACQ_REL`）；`handle_cow_fault` 对 fork 路径正确（copy-before-dec + 各自 PTE），无需改。

### 修

fork() 页表遍历后 + clone() `cow_clone_address_space()` 遍历后各加一次 `flush_tlb_all()`（cr3 重载，O(1)，刷掉父 CPU 陈旧 writable 项）：

- [fork.cpp](../../kernel/proc/fork.cpp) L279（PML4 遍历 `copy_page_table_level` 之后、VMA 克隆之前）。
- [clone.cpp](../../kernel/proc/clone.cpp) L115（同上）。

**局部刷足够**：fork 在 IF=0 下跑（SFMASK 清 IF，dispatch 前 no `sti`），父不可被抢占/迁移，cr3 仍在该 CPU；子的 cr3 全新从未加载（首跑 clean TLB）。跨核 TLB shootdown（CLONE_VM 线程才需要）是单独 follow-up，fork 路径不需要。

## 根因 2（pre-existing，已修）：syscall_entry sysretq 不恢复用户 RBP

复现器跑起来先撞到一个**独立的、pre-existing、非 SMP** 的 bug：syscall.S 的退出路径恢复了用户 RBX（frame+80）却**漏了 RBP**（frame+88，入口 L64 `pushq %rbp` 存了）。RBP 是 SysV ABI callee-saved，内核必须跨 syscall 保留。任何用帧指针跨 syscall 的用户代码（musl 编译产物、手写 sys_fork）返回后 RBP 是垃圾 → 首个 `[rbp+off]` 访问 segfault。

shell / hello.c 没炸是因为它们的 syscall wrapper 经 gcc 优化后不跨 syscall 用 rbp；F10 musl 移植迟早会踩到。

### 修

[syscall.S](../../kernel/arch/x86_64/syscall.S) 退出路径，RBX 恢复后加 `movq 88(%rsp), %rbp`（恢复用户 RBP）。trap frame 早已存了，退出只是漏读。

## 复现器：tools/musl/forktest.c

musl 静态用户程序，**裸 SYS_fork(57)** 循环（= shell launch_program 的精确路径）。每轮：fork → 父立即写共享 CoW 全局 `g_marker` → 子读 `g_marker`。CoW 正常则子见 pre-fork 值（clean）；父写穿透陈旧 TLB 则子见父的写（race）。`FORKTEST iters=N races=M clean=K` 走串口。

装成 `/hello` 让 F10-M1 ring-3 smoke（`CINUX_MUSL_HELLO_SMOKE=ON`）在 -smp 2 下启动它。smoke worker 是无 AS 内核线程，fork 不走 CoW；它 fork 出的子 execve /hello=forktest 后，**forktest 自己是 user task，它的 fork 才走 CoW**。

构建/运行（KVM 才有真 TLB/SMP）：
```
tools/musl/build-hello.sh 风格 gcc … tools/musl/forktest.c -o build/musl/forktest
scripts/create_ext2_disk.sh build/ext2.img build/user/shell build/musl/forktest   # forktest 当 /hello
# 跑 -smp 2（QEMU 命令同 run-kernel-test-smp，ext2 换成上面那个）
```
`-DFORKTEST_ITERS=N` 调迭代数；`-DFORKTEST_NO_PARENT_WRITE` 关父 post-fork 写（诊断用）。

## 实证（before/after，ITERS=1 = shell 单 fork 路径）

| 树 | ITERS=1, -smp 2, 3 runs |
|----|-------------------------|
| **未修 CoW**（fork/clone 无 flush，rbp 修保留） | run1 `races=1`（抓到写穿透），run2/3 clean（偶发，合 note「偶发」） |
| **全修**（flush + rbp） | 3/3 `races=0 clean=1` PASS |

→ CoW flush 修复**有效且必要**；单 fork（shell 启动 /hello 的路径）-smp 2 下 CoW 隔离正确。

## 残留（未修，登记 follow-up）

**fork#2 rbp 腐蚀**：`ITERS>=2` 时父在第 2 次 fork 的 sys_fork 返回处 segfault（rbp=0x13）。诊断：rbp 在用户态被腐蚀（frame+88 入口即 0x13），非信号（崩前无 `[SIGNAL]`）、非 PF handler（interrupts.S 全 GPR 存/恢复）、非 syscall 恢复（rbp 修在）。触发与「CoW fault + reap 后再次 fork」相关；**不影响单 fork**（shell 正常启动 /hello 不触发）。需后续单独排查（疑 wait4/yield 路径或某 CoW 后状态）。

## 验证

- 默认 `run-kernel-test` **954/0**；`run-kernel-test-smp`（-smp 2）**954/0**（全修后无回归）。
- forktest ITERS=1 -smp 2 全修 3/3 races=0；单 CPU 同。
- 「单 fork = shell 启动路径」已稳；`make run` GUI shell 敲 /hello 是最终实跑确认（headless 复现器已证 CoW 隔离）。

## commit

- `fix(arch): syscall_entry sysretq 恢复用户 RBP`（syscall.S）
- `fix(smp): fork/clone CoW 改写父页表后刷 TLB`（fork.cpp + clone.cpp + 清 syscall.cpp 诊断打印）+ 复现器 + 本文
