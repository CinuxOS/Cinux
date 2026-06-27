# F10 follow-up：AddressSpace 析构误释放 CoW 共享叶子页

> 分支 `feat/f10-musl`。接 `2026-06-27-smp-shell-corruption-handoff.md`。

## 背景

`ea4520e`（syscall 恢复用户 RBP）和 `6a7beb9`（fork/clone CoW 后 flush TLB）修掉了两个真实 bug，但 GUI shell / forktest `ITERS>=2` 仍在第二次 fork 后崩。症状看起来像用户态 RBP/返回链被写花：`rip=0x401182`，fault addr `0xb`。

## 定位

先按交接建议试过 `handle_cow_fault` 只降 mapcount 不释放 old phys，单 CPU `forktest ITERS=2` 仍崩，排除“跨核 CoW free UAF”作为单核确定性复现的主因。

随后给 fork syscall entry/exit 临时 trace，确认第二次 fork 的 syscall trap frame 里 user RBP 在入口和出口都正确；再在 user #PF 处临时 dump `rip` 附近字节，发现 `0x401182` 所在代码页内容已经变成 `0x0000000000000000`。CPU 实际执行的是被清零后的假指令（`00 00`，写 `[rax]`），而 `rax=11`（fork 返回的 child pid）所以 fault addr 是 `0xb`。

根因在 `AddressSpace::free_subtree()`：注释说 PT 层“PT entries point to data pages, not owned by address-space infrastructure”，但代码仍在循环尾部无条件 `g_pmm.free_page(table[i].phys_addr())`。也就是 AddressSpace 析构时把叶子 PTE 指向的用户数据页当成页表页释放。

fork child 退出后被 `waitpid()` reap，`delete Task -> delete AddressSpace -> free_subtree()` 释放了 child 的 text/data/stack 物理页；这些页仍被 parent 映射（CoW/shared），于是 parent 后续继续执行的 text page 被 PMM 复用/清零，表现为用户代码页腐蚀。

## 修复

`AddressSpace::free_subtree()` 在 `LEVEL_PT` 只处理叶子 data page：

- `mapcount_dec_and_test(data_phys)`
- 只有最后一个映射才 `free_page(data_phys)`
- 清空 PTE 后 `continue`

中间层仍递归释放子页表，再释放页表页本身。

这让 AddressSpace 析构与 `execve::clear_user_mappings()` 的 CoW mapcount 语义一致，避免 reaped child 释放 parent 仍映射的共享页。

## 验证

当前环境的 CMake QEMU target 带 `-vnc :0`，sandbox 不能 bind socket；因此使用同一 CMake 产物和磁盘布局，改 `-display none` 直跑 QEMU。直跑 isa-debug-exit 成功码表现为 QEMU exit `1`（wrapper 平时会映射）。

- `forktest.c -DFORKTEST_ITERS=2` 单 CPU：`FORKTEST iters=2 races=0 clean=2 errs=0`，smoke PASS。
- `forktest.c -DFORKTEST_ITERS=2` `-smp 2`：`FORKTEST iters=2 races=0 clean=2 errs=0`，smoke PASS。
- 恢复普通 `build/musl/hello` 后 `-smp 2`：`Hello from musl on CinuxOS!`，`smoke: hello exit_status=0 ... PASS`，内核测试 `954 passed, 0 failed`。

## 仍需实机确认

这里确认了 headless smoke / forktest 路径。GUI shell 交互（`cmake --build build --target run`，点 Shell，输入 `/hello`）还需要在可打开 VNC/GUI 的本机环境再点一次；按根因看应已覆盖同一 fork+execve+reap 机制。
