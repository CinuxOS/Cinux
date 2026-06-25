# F-CLN 批8 — 收官 — 2026-06-25

> F-CLN 债务清理里程碑收官。批0-8 全 ✅。文档同步（ROADMAP/PLAN/debt）+ 验证矩阵。

## 验证矩阵（全绿）
- run-kernel-test **931/0**
- **-smp 2** ALL PASSED（SMP 债：018 kMaxCpus / 010 FDTable refcount / 007 quantum）
- host ctest **54/0**（100%，Task 字段加 + ASSERT_OK host 改动回归）
- **LOCKDEP 931/0 零误报**（批4 vma_lock irq_guard / 批7 task_tick irq_guard，短临界区不引入 lockdep 误报）

## F-CLN 全景（10 commit，feat/f-cln-debt）
| commit | 批 | 内容 |
|---|---|---|
| aa2b740 | 立项 | PLAN/ROADMAP F-CLN 段 |
| 8ccbaf7 | 0 | xHCI/USB 专项审 → D2/D4 清洁，D3 发现 **DEBT-021**（P1，poll_events 并发，登记留 xHCI 重构）|
| 91e4afd | 1 | DEBT-015 sys_dmesg 栈→堆 + frame 门禁（GCC -Werror= 限制诚实记录）|
| 4604125 | 2 | DEBT-016 ASSERT_OK 宏 + 清 32 处 ErrorOr 忽略 + 去 -Wno-unused-result |
| 25ccd83 | 3 | DEBT-018 acpi kMaxCpus→kMaxAcpiLapics + static_assert |
| d888052 | 4 | DEBT-008 signal_setup_frame 校验栈 VMA（中风险，信号路径）|
| 018b200 | 5 | DEBT-009 clear_user_mappings + free_subtree huge 检测（防御）|
| 65de657 | 6 | DEBT-010 FDTable refcount atomic 对齐 R3 |
| 9f9c311 | 7 | DEBT-007 quantum per-task（中风险，调度核心，对齐 Linux time_slice）|
| (本次) | 8 | 收尾：ROADMAP/PLAN/debt ✅ + 验证矩阵 |

## 残留 open（登记，不阻塞 F-CLN）
- **DEBT-019/013/020/012**（用户指针 + ELF，P3）：留 F10 libc 顺手修（与 syscall/execve 强相关，F10 时更自然）
- **DEBT-011/014**（slab 双释放启发式 / no_reschedule_depth，P3）：低危，排期
- **DEBT-021**（xHCI poll_events 并发，P1）：留 xHCI/输入子系统重构（加 irq-safe Spinlock）

## 产出
- 7 债闭环（015/016/018/008/009/010/007）+ 1 新债登记（021）
- ROADMAP/PLAN/debt 全标 ✅ + 8 篇批 notes
- feat/f-cln-debt 分支 10 commit 待 PR

F-CLN 收官。下个大弧：F10 libc/musl（syscall 矩阵 + ELF 动态链接）+ F7 网络（E1000 + 协议栈 → ping）并行。
