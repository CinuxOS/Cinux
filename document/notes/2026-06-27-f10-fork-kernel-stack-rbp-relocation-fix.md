# F10 follow-up：fork/clone 拷贝内核栈后未重定位 RBP 链

> 分支 `feat/f10-musl`。接 `2026-06-27-f10-address-space-free-subtree-cow-fix.md`。

## 背景

修掉 `AddressSpace::free_subtree()` 误释放 CoW 共享叶子页后，headless
forktest 与 musl hello smoke 已稳定，但 GUI shell 路径仍在 `-smp 2` 下崩。

最新 `log.txt` 的关键症状：

- shell fork child `tid=7`
- child 第一次用户栈 CoW 后进入内核
- kernel-mode #GP，`RIP=0xCCCCCCCCCCCCCCCD`
- `RSP=0xFFFF80000B21DF90`，落在 shell 父任务内核栈附近

这已经不像用户 text/data 页被提前释放，而像 fork child 从复制来的内核栈返回时，沿着父内核栈退栈。

## 根因

`fork()` / `clone()` 复制 parent 当前内核栈后，只把 child 的入口 `ctx.rsp`
平移到 child stack：

```cpp
child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;
child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);
```

问题是被复制的栈帧里，saved RBP 链仍然保存 parent 内核栈地址。

实际返回路径是：

1. `context_switch` 载入 child ctx，跳 `fork_child_trampoline`
2. trampoline `rax=0; ret`，回到复制出来的 `sys_fork`
3. `sys_fork` `pop %rbp` 后，RBP 变成复制栈里保存的 **parent `syscall_dispatch` RBP**
4. `syscall_dispatch` 末尾用 parent RBP 做 `mov -8(%rbp),%rbx; leave; ret`
5. child 的 RSP 被 `leave` 切到 parent 内核栈，最终 `ret` 到填充值 `0xCCCC...`

这正好解释 GUI log 里 child 当前任务是 `tid=7`，但崩溃 RSP 落在 parent shell 内核栈附近。

## 修复

新增 `prepare_copied_kernel_stack_context()`，fork/clone 共用：

- `ctx.rsp` 仍指向 child 栈中的 fork return address
- `ctx.rbp` 从 parent saved RBP 重定位到 child 栈
- 沿 `current_rbp` 的 frame-pointer chain 遍历实际复制的源栈区间，把每个 copied frame 的 saved RBP 从 parent 地址重写成 child 地址
- 若 saved RBP 不在复制区间内则保留原值并停止，避免误改非栈指针

同时修正 `clone()` 同类隐患；clone child 也经同一个 trampoline/复制内核栈路径返回。

## 回归钉子

新增/增强测试：

- `test_fork_exec.cpp`：`sys_fork()` 直接路径和经 `syscall_dispatch()` 路径，在 quarantine child 前检查 `ctx.rsp`、`ctx.rbp`、下一层 saved RBP 均落在 child kernel stack。
- `test_clone.cpp`：`clone(CLONE_VM|CLONE_THREAD)` 路径同样检查 copied RBP chain 已重定位。

旧代码会让下一层 saved RBP 指回 parent/test 栈；新测试能在 child 被真正调度前抓住。

## 验证

本 sandbox 不能 bind CMake target 的 `-vnc :0`，所以 `run-kernel-test` 的构建阶段过，但 QEMU 直跑改用 `-display none`。

- `timeout 40 cmake --build build --target run-kernel-test -j$(nproc)`：编译、链接、打 test image 成功；QEMU 因 `-vnc :0` 受限，不能作为本环境真运行结果。
- headless 单 CPU test image：`954 passed, 0 failed`，musl `/hello` fork+exec+wait smoke PASS。
- headless `-smp 2` test image：`954 passed, 0 failed`，musl `/hello` fork+exec+wait smoke PASS。
- `cmake --build build --target image -j$(nproc)`：生产 `build/cinux.img` 已重建成功。

## 仍需 GUI 复验

这次根因与 `log.txt` 的 `RIP=0xCCCC...` / child 沿 parent kernel stack 返回一一对应。仍需在可打开 VNC/GUI 的本机环境跑：

```bash
cmake --build build --target run -j$(nproc)
```

点 Shell，输入 `/hello` 或等价 musl 程序路径，确认不再 #GP/panic。

## 第二关：execve path=0

GUI 复验后不再 #GP，但 `log.txt` / `log2.txt` 显示 child 并没有真正 exec
`/hello`：

```text
[EXECVE] sys_execve path=0x0000000000000000 argv=... envp=...
[EXECVE] copy path_n=0 argc=1 envc=0
[EXECVE] copy failed -> EINVAL
[SYSCALL] sys_exit(127)
```

shell 的 `launch_program()` 被优化后把 `argv[0]` 缓存在用户态 `r12`：

```asm
mov    -0x190(%rbp),%r12
call   sys_fork
...
mov    %r12,%rdi
call   sys_execve
```

普通 syscall 没切任务时，kernel C 函数按 SysV ABI 保持 `r12`，所以问题不明显。
但 fork child 是从复制的 kernel stack 和 `Task::ctx` 恢复出来的；`syscall_entry`
原 frame 只保存/恢复 `rbx/rbp`，没有保存用户 `r12-r15`。child 返回用户态时
`r12` 来自陈旧 `Task::ctx`，于是 path 变成 0。

修复：把 `syscall_entry` frame 从 96B 扩到 128B，保存/恢复所有 SysV
callee-saved 用户寄存器：

- `r12/r13/r14/r15`
- `rbx/rbp`

同步把 clone 的 `kSyscallFrameSize` 从 96 改为 128，保持 child user-RSP
patch 仍指向 frame offset 0。

新增验证：

- headless 单 CPU test image：`954 passed, 0 failed`，musl `/hello` smoke 输出 `Hello from musl on CinuxOS!`。
- headless `-smp 2` test image：`954 passed, 0 failed`，musl `/hello` smoke 输出 `Hello from musl on CinuxOS!`。
- `cmake --build build --target image -j$(nproc)`：生产 `build/cinux.img` 已重建。
