# 2026-07-02 — 批4-a GCC 工具链(glibc 动态)as+ld 自举闭环

> Feature F12-M2(GCC 自举)第一刀。分支 `feat/b4-gcc-toolchain`(从 main `0b82e1b`)。
> 6 commit:c7c1ff5(B0 内核 glibc 兼容)/ 5bb319d(B1a mkfs-d 装盘)/ db5b0b2(B1b gcc 装盘)/ 3832c45(B2 as --version + pread64)/ 2c4bfc8(B2 as hello.s)/ 270daf0(B3 ld+hello 自举)。

## 用户决策校正(2026-07-02,推翻旧 musl 静态自编方向)
1. **改 glibc**(非 musl):兼容最广,对齐发行版。杠杆在 PT_INTERP 是 libc 切换缝 —— kernel 只认 Linux ABI,F10-M2 动态加载机制复用。
2. **不自编工具链**:从本机系统(gcc 16.1.1 + binutils 2.46,glibc 动态)提取现成 as/ld + 运行时 + crt + libgcc 装盘。
3. **先 as+ld 再 cc1**:cc1(47MB 大 ELF + 大 mmap)最不可控,隔离到 B4-b。
4. **musl/glibc 共存**(首选):进程间零风险(内核中立),保 busybox -O2 musl ABI 试金石;回退条件 = glibc 跑不通。

## 成果:自举闭环两腿 PASS
- **glibc 动态 ELF 第一次在 CinuxOS 跑通**:`as --version` 输出 "GNU assembler 2.46.0" + exit 0(两腿)。
- **as+ld 自举**:`as /hello.s -o /hello.o`(汇编+写 .o,ext2 写验通)→ `ld … -o /hello`(链接)→ `./hello` 跑 printf "Hello from GCC!"(两腿)。**hello 由 CinuxOS 上 as+ld 编出,在 CinuxOS 跑通 = 自举闭环**。
- 目标 `gcc hello.c -o hello && ./hello` 留 B4-c(需 cc1 + headers + gcc driver,B4-b 装)。

## 改动

### B0 内核 glibc 兼容 4 处(commit c7c1ff5)
glibc ldso 比 musl 娇气,补 auxv + 栈 + brk:
- **auxv +AT_PLATFORM**("x86_64")/ **AT_HWCAP**(CPUID.01H:EDX):`initial_stack.hpp` build_initial_stack helper 内部塞(同 EXECFN/RANDOM 模式)+ `user_launch.cpp` AT_HWCAP 填 `cpuid::hwcap_from_cpuid()`;新 `kernel/arch/x86_64/cpuid.hpp`。
- **execve 扫 PT_GNU_STACK**:栈默认 NX(glibc 期望,modern gcc PT_GNU_STACK=RW);legacy RWX 才 Exec。`ElfAuxInfo.stack_executable` 字段 + `execve.cpp` 扫 `kPtGnuStack=0x6474e551` + `user_launch.cpp` 栈页/VMA flags 按 stack_executable。
- **USER_BRK_MAX 64MB→240MB**:留 ldso(@256MB)gap;as/ld/cc1 堆。动态增长 / 动态 ldso 基址留 follow-up。
- vDSO 显式不做(glibc fallback syscall,慢不死)。
- host 69/69(AT_PLATFORM 新断言)+ 两腿 ALL PASSED。

### B1 装盘(commit 5bb319d + db5b0b2)
- **B1a `create_ext2_disk.sh` 切 `mkfs.ext2 -d` 目录树**:取代 debugfs 逐文件 write/ln(无法扩展到 ~10k GCC headers)。mkfs -d **保留 hardlink**(验证 busybox 30 applets 同 inode Links:30)+ 保 SONAME 软链(cp -a)。busybox hardlink 模型迁移(debugfs ln → 目录树 ln)。
- **B1b `tools/gcc-toolchain/extract.sh`**:host 提取 as/ld + ldd 依赖闭包(libc/libm/libbfd/libctf/libjansson/libz/libzstd/libsframe/libgcc_s + ldso)+ crt(crt1/Scrt1/crti/crtn + crtbegin/S/T/crtend/S)+ libgcc.a/eh,布局对齐 GCC specs 硬编码路径;不含 cc1/headers(B4-b)。hello.s host `gcc -S -fno-pie` 预编(non-PIE,对齐 F10-M2 non-PIE ET_EXEC)。
- **qemu.cmake `CINUX_GCC_TOOLCHAIN` option**(默认 OFF,CI 无 GCC-private crt;本地 ON):extract.sh 产 gcc-root + 盘扩 128MB/inode 8192 + cp -a 合并。
- ⭐**GOTCHA**:`run-kernel-test` 依赖 `regenerate-ext2-image` target(qemu.cmake),它自己跑 create_ext2_disk.sh **但原漏传 GCC_ROOT**(只 7 参)→ run-kernel-test 重建 8MB busybox 盘无 gcc。修:regenerate-ext2-image 也传 GCC_ROOT + IMAGE_SIZE/INODES env + DEPENDS gcc-root.stamp。

### B2 as 冒烟 + pread64(commit 3832c45 + 2c4bfc8)
- **测试内核加 as smoke**(main_test.cpp,gate `CINUX_GCC_TOOLCHAIN`):fork+execve /usr/bin/as(首个 glibc 动态 ELF)。3 处 smoke gate(line 163/555/945)+ exit_code 加 as。
- **`kernel/CMakeLists.txt`**:`CINUX_GCC_TOOLCHAIN` → big_kernel_common compile_definitions。
- **sys_pread64(17)**:glibc ldso 精读 ELF 段用。补前 as `exit 127`(ldso 失败码),补后 PASS。参照 sys_read,offset 参数不改 fd offset。syscall_nums/syscall.cpp 注册 + CMakeLists。
- as --version PASS → 升级 `as /hello.s -o /hello.o`(汇编+写 .o,ext2 create+write)两腿 PASS。

### B3 ld+hello 自举(commit 270daf0)
- **ld smoke**:ld 链接 hello.o→hello(non-PIE crt1/crti/crtbegin + -lgcc -lc + crtend/crtn,-dynamic-linker glibc ldso)。crtbegin 路径 GCC-private 16.1.1 硬编码。
- **hello smoke**:`./hello` 跑 printf "Hello from GCC!" = 自举证明。两腿 PASS。
- ⭐**ld exit SIGSEGV follow-up**:ld 链接成功产 hello + hello 跑通,但 ld **exit cleanup** 崩(addr 0x240613308 mmap arena has no VMA,9GB mmap 区)。gate 用 as + ./hello(自举),ld exit 崩留 B4-b(cc1 也驱动 ld,可能与 mmap 大 arena demand-paging 同根)。

## 关键 GOTCHA
1. **run-kernel-test 走 regenerate-ext2-image**(非 add_custom_command ext2.img 那套)—— 改装盘脚本要两处同步(create_ext2_disk add_custom_command + regenerate-ext2-image target)。
2. **mkfs.ext2 -d 保留 hardlink**(e2fsprogs 1.47.4 验证)—— busybox 30 applets 同 inode,盘省空间。
3. **non-PIE 对齐 F10-M2**:host gcc -S 默认 PIE;`-fno-pie` 产 non-PIE hello.s,ld `-no-pie` + crt1.o(非 Scrt1)链 non-PIE ET_EXEC(已验)。PIE main(ET_DYN)留 ELF-base ASLR follow-up。
4. **glibc 启动探测 syscall 容忍 ENOSYS**:unhandled 273/334/302/318(io_uring/rseq/prlimit64/getcpu)= glibc 启动探测,ENOSYS 容忍,as 照常。cc1(B4-b)可能更敏感,看是否需补。
5. **VNC 避让**:多会话共用 -vnc :0 互杀;`sed -i 's/-vnc :0/-vnc :5/g'` 跑完 `sed -i 's/-vnc :5/-vnc :0/g'` 还原(因本批改了 qemu.cmake,不能用 git checkout 还原)。
6. **ld exit 崩**:addr 0x240613308 mmap arena;链接成功(产 hello)+ hello 跑通,exit cleanup 问题。B4-b follow-up。

## 验证
两腿 run-kernel-test-all ALL PASSED + busybox 14/14 + as --version PASS + as hello.s PASS + ld(FAIL exit 崩但产 hello)+ ./hello PASS(两腿)。host 69/69。

## 后续(B4-b / B4-c)
- **B4-b cc1**:装 headers(249MB,盘扩/inode 25600)+ cc1(47MB 大 ELF);`gcc -c hello.c -o hello.o`(大 ELF 加载 + 大 mmap 工作集 + mremap/getrusage 缺口)。ld exit 崩在此批定位(可能 mmap 大 arena)。
- **B4-c 完整自举**:`gcc hello.c -o hello && ./hello`(gcc driver fork cc1→as→ld 管道/临时文件链)+ 压测。
- follow-up:brk 动态增长 / 动态 ldso 基址(堆 >240MB)/ vDSO / AT_HWCAP2 / PIE main ELF-base ASLR / ld exit 崩。
