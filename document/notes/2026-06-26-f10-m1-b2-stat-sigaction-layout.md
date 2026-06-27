# F10-M1 批2 — struct stat / sigaction 布局对齐 Linux x86_64 ABI

> 里程碑：F10-M1 用户态运行时 / musl 静态移植（批2）。分支 `feat/f10-musl`，commit `40be22e`。
> 验证：g++ 探针逐字段 offset 对齐 Linux UAPI + 全量编译绿 + `run-kernel-test` 945/0。

## 背景与方法

批1 把 syscall 号和返回约定对齐了 Linux。批2 管**结构体布局**——musl/glibc 静态二进制
直接按 Linux 内核 ABI 读写 `struct stat` / `struct sigaction`，字段顺序、大小、padding 错一个
字节都会读乱。

**关键方法（用户建议，效果好）**：不猜，拉源码实测对照——

- **musl 1.2.5**（`/tmp/musl`，release tarball）：x86_64 上 musl 直接用内核 `struct stat`，无转换；
  `src/signal/sigaction.c` 展示它构造的内核 sigaction 是 `{handler, flags, restorer, mask}`。
- **Linux UAPI**（本机 `/usr/include/asm/stat.h`、`asm-generic/signal.h`）：内核必须提供的真身。
- **glibc 源码**（`/tmp/glibc`，浅 clone）：长弧参考（后续 glibc 静态二进制兼容 stretch）。
- **g++ 探针**（`/tmp/stat_layout_probe.cpp`）：把 CinuxOS struct 和 `<asm/stat.h>` 的 `struct stat`
  放一起，`static_assert(sizeof)` + 逐字段 `offsetof` 比对——硬证对齐，不靠人眼。

## 差距（靠读源码发现，靠猜必翻车）

### struct stat —— 差得最多

Linux x86_64 `struct stat`（144 字节，uapi/asm/stat.h）vs 原 CinuxOS `sys_stat`（~96 字节）：

| 项 | Linux | 原 CinuxOS |
|----|-------|-----------|
| 字段顺序 | `st_ino → st_nlink → st_mode` | `st_ino → st_mode → st_nlink`（**反了**）|
| st_nlink | `unsigned long` 8 字节 | `uint32_t` 4 字节 |
| __pad0（gid 后）| 有（对齐 st_rdev 到 offset 40）| 无 |
| 时间 | `st_atime + st_atime_nsec`（sec/nsec 拆 2×8，共 6 字段）| 单 `st_atime/mtime/ctime` |
| __unused[3] | 有 | 无 |

### struct sigaction —— 顺序全错

Linux 内核 `struct sigaction`（asm-generic/signal.h）= `{sa_handler, sa_flags, sa_restorer, sa_mask}`。
原 CinuxOS `UserSigAction` = `{sa_handler, sa_mask, sa_flags, sa_restorer}`——**完全错位**，musl 按
Linux 序构造、内核按 CinuxOS 序读，会拿 flags 值当 mask、restorer 当 flags。

### sigset —— 已合规

`SigSet = uint64_t`（8 字节）= 内核 sigset（rt_sigaction/rt_sigprocmask 第 4 参 sigsetsize =
`_NSIG/8 = 8`）。musl 用户态 sigset_t 是 16 字节，但只在内核边界用 8 字节。**不用改。**

## 改动

- `kernel/fs/stat.hpp` + `user/libc/syscall.h`（`sys_stat`）：重写为 Linux 144B 布局。
- `kernel/syscall/sys_signal.hpp`（`UserSigAction`）+ `user/libc/syscall.h`（`sys_sigaction`）：
  重排为 `{handler, flags, restorer, mask}`。
- `kernel/syscall/sys_signal.cpp` rt_sigaction：接入 `sa_flags` → `SA_RESTART`/`SA_RESETHAND`
  （原先字段在、却总被忽略，等于白装）。SA_RESTORER 不接——CinuxOS 注入自己的 sigreturn
  trampoline（F9 批1 vDSO @0x100000），不用用户 restorer。
- `kernel/fs/ext2_common.cpp`（Ext2FileOps/Ext2DirOps 两处 `stat()`）：开头加 `memset(st, 0,
  sizeof(*st))`，新 padding/`_nsec`/`__unused` 字段不漏内核栈字节给用户态。

shell 的 `cmd_stat` 按字段名读（`st_size`/`st_ino`/`st_nlink`/`st_mode`…），名字都在、无需改。

## 验证（g++ 探针 = 硬证）

```
stat:        cinux sizeof=144  linux sizeof=144  (144 expected)
sigaction:   sizeof=32  handler@0 flags@8 restorer@16 mask@24
LAYOUT ALIGNED with Linux x86_64 UAPI      (exit 0)
```

stat 16 个字段 `offsetof` 逐一对齐 Linux UAPI（无 MISMATCH）；sigaction 32B + 字段序正确。
加全量编译绿 + `run-kernel-test` 945/0。

## 陷阱

- **`cd /tmp` 持久化 cwd**：clone musl 时 `cd /tmp && ...`，把 Bash 工具的持久工作目录切到
  /tmp，后面相对路径的 `grep kernel/...` 全在 /tmp 下找（空结果）。修：用绝对路径 / `cd` 回仓库。
- **VNC 5900 偶发占用**：run-kernel-test 的 QEMU 用 `-vnc :0`（qemu.cmake 硬编，无 env 覆盖），
  Windows 侧/外部占用 5900 时 QEMU 起不来（不报 FAIL，是没输出）。重试或换端口（F9 批9 同坑）。
- **`timeout 40` 偶尔不够**：run-kernel-test target 含 ext2 重建 + QEMU 冷启动，系统忙时 40s
  跑不完 945 项（中途被杀，显示到 600+ 就断，不是失败）。给 90s 稳。

## 下一步

批3：execve 压 auxv 辅助向量（AT_PHDR/PHNUM/PAGESZ/RANDOM/ENTRY…）——musl `__init_libc` 启动
必读，目前 grep 全空，是 musl 起不来的头号拦路虎。照批1/批2 的方法：读 musl `src/env/` +
`__init_libc` 看它到底读哪些 AT_* 键，按需压栈。
