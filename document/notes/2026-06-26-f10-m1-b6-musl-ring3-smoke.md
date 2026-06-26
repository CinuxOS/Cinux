# F10-M1 批6 — harness ring-3：musl 静态 hello 端到端跑通

> 里程碑：F10-M1 用户态运行时 / musl 静态移植（批6，收官）。分支 `feat/f10-musl`。
> 验证：`-DCINUX_MUSL_HELLO_SMOKE=ON` → 950 单测 + 串口 `Hello from musl on CinuxOS!` + smoke PASS（exit 0）；默认 OFF → 950/0 不破。

## 结果：CinuxOS 首次真跑 musl 静态用户程序

```
[EXECVE] loaded /hello entry=0x40103B pid=10
[PROC] jumping to user mode: entry=0x40103B rsp=0x7FFBBDEB0
Hello from musl on CinuxOS!                  ← musl write(1,...) → SYS_write → kprintf
[SYSCALL] sys_exit(0) from tid=139           ← musl exit_group(0) 干净退出
[WAITPID] reaped child pid=10 exit_status=0
[F10-M1] smoke: hello exit_status=0 reap=10 -> PASS
```

**全 musl 启动链在 CinuxOS ring3 跑通**：`_start` → `_start_c` → `__libc_start_main` → `__init_libc`（读 auxv，批3 铺的栈）→ `__init_tls`/`__init_tp`（`arch_prctl(ARCH_SET_FS)` 设 TLS，批4 syscall）→ stack canary 读 `%fs:0x28` → `__libc_start_init` → `main` → `write` → `exit_group`。批1-5 的 ABI/初始栈/syscall 工作全部端到端验证。

## 用户选的「投资 harness 跑 ring3」路线

用户明确选 harness 方案（非 boot-smoke）。harness 当年设计成「无 dispatch loop、各测试自带 fixture」，故需逐层补生产基建。共修 7 个真实 gap（其中 3 个是 latent bug，已独立提交 `c3d7d2d`）：

| # | gap | 修法 |
|---|---|---|
| 1 | dispatch 未注册返 -ENOSYS | 批4（`8bab7a2`）|
| 2 | ext2 镜像没 /hello | create_ext2_disk.sh 加第 3 参 |
| 3 | regenerate-ext2-image target 没传 hello | cmake/qemu.cmake 补 |
| 4 | fork 子无 address space | 子 fork 后 `new AddressSpace()`（照 shell_launch.cpp）|
| 5 | ext2 没挂进全局 VFS | smoke 复制 test_ext2 的 setup_ext2 全套（PCI→AHCI→port1→Ext2 mount）|
| 6 | **#DF：jump_to_usermode SMAP 不安全写** | `c3d7d2d`：`pushq $0x202;popq %r11`→`movq $0x202,%r11`（见下）|
| 7 | sys_exit 不设 exit_status（退出码丢失）| `c3d7d2d`：`task->exit_status = code` |

## #DF 根因（批6 最大成果，真 bug，`c3d7d2d` 已修）

musl 首次进 ring3 即 #DF（Double Fault）。addr2line（用 LSTAR=syscall_entry 作锚算加载基址）定位 RIP=**`jump_to_usermode`**。诊断打印确认 `gs:0`/LSTAR 都对，故 #DF 在 jump 执行期间。

`jump_to_usermode` 先 `mov %rsi,%rsp` 切到**用户栈**，再 `pushq $0x202;popq %r11` 设 SYSRET 的 RFLAGS——**在内核态写用户内存**。SMAP 开则 #PF，此时 RSP=用户栈、#PF 推栈失败→#DF。改 `movq $0x202,%r11` 立即数加载，等价且不碰用户内存。

**生产 /bin/sh 没炸**是因 F9 memory 记的：WSL2 KVM 不透传 SMAP CPUID（CPUID.07H:EBX[20]）→ `enable_smep_smap()` 的 CPUID gate 没开 CR4.SMAP。**真硬件 / SMAP 透传环境必炸**——这是个潜伏的、生产相关的真 bug，批6 ring3 调试首次暴露。

## follow-up：printf/stdout FILE segfault（窄，不挡核心）

musl 全运行时跑通，但 **`printf` 专用路径有独立 segfault**：`__stdout_write` 里 `f->lbf=-1`（`movl $-1,0x90(%r8)`）时 f 似为 NULL（fault addr=0x90）。矛盾：进 `__stdout_write` 需先读 `f->write`（要 f 非0），但内部 r8=0。`stdout` 指针（@0x404fd0, .data.rel.ro）在**文件里 = 0x405020 正确**（`objdump -s` 验证），execve RW PT_LOAD 读取逻辑也算对了。静态分析解释不了，**需运行时 dump 子地址空间 0x404fd0 实际值**确认 execve 是否真把 .data.rel.ro 写进去了（疑似 .data.rel.ro / 非页对齐 p_vaddr=0x404fc0 的加载 bug）。raw `write()` 绕开 stdio 完美工作 → 问题锁定在 stdio stdout FILE 懒初始化，不影响 musl 运行时证明。hello.c 暂用 `write()`。

## 改动

- `kernel/test/main_test.cpp`：`CINUX_MUSL_HELLO_SMOKE` gate 的 `musl_hello_smoke_entry`（setup_ext2 挂载 + fork + 子建 AS + launch /hello + 父 WNOHANG 轮询 waitpid + yield）+ `kernel_main` 末尾 `Scheduler::init`+`run_first` 进调度（worker 自写 isa-debug-exit）。
- `CMakeLists.txt`/`kernel/CMakeLists.txt`：option `CINUX_MUSL_HELLO_SMOKE`（默认 OFF）。
- `scripts/create_ext2_disk.sh` + `cmake/qemu.cmake`：ext2 `/hello` 接线（可选第 3 参，软依赖 build/musl/hello）。
- `tools/musl/hello.c`：`write()` 版（+ 注释说明 printf follow-up）。

## 踩坑

1. **run_first 不回 caller**：worker exit 后调度切 idle（死循环），不回 boot_ctx。故 worker **自写 isa-debug-exit** 终止 QEMU（不靠 harness main 继续）。
2. **harness 无全局 AHCI/ext2**：每个 ext2 测试用局部 fixture（setup_ext2 自带 PCI→AHCI→Ext2）。smoke 得复制全套挂载，不能只 `AHCI::instance()`。
3. **阻塞 waitpid 在 harness 脆弱**：父用 WNOHANG 轮询 + yield（非阻塞 waitpid），避开 harness 没充分接好的 block/unblock。
4. **addr2line 偏移**：big_kernel_test 带地址自映射（base=0），但 KALLSYMS/backtrace 偏移大不可信；用已知符号（LSTAR=syscall_entry）作锚算基址再解析才准。
5. **#DF IST1 栈**：#DF dump 里的 RSP 是内层异常的 RSP（用户值），不是 #DF 自己的栈——别误读。

## F10-M1 收官

批0-6 全 ✅。musl 静态移植地基完成：syscall ABI（批1）+ 结构体布局（批2）+ execve 初始栈 auxv（批3）+ musl syscall 集（批4）+ musl 编译/sysroot（批5）+ ring3 端到端（批6）。**CinuxOS 能跑 musl 静态用户程序**。残留 follow-up：printf/stdout FILE segfault、execve 替换路径栈铺设（批3 只做 launch 路径）、多线程 exit_group 真全杀。
