# F4 follow-up — SMP 任务迁移竞态修复(task on_cpu)

> 里程碑:F4 follow-up(SMP 调度加固,插队 F-GUI-DECOUPLE 之前)。分支 `feat/smp-migration-fix`(从干净 main `1e225ff`)。commit `68b1913`。
> 来源:F-GUI-DECOUPLE 批2(launch_userspace 抽函数)触发 `make run -smp 2` 必现 panic,诊断为预存 SMP bug,用户决策「先定位再决策」→「开 F4 follow-up 修迁移同步」。

## 背景:F-GUI-DECOUPLE 批2 触发

批2 把 `kernel_init_thread` 的「GUI 启桌面 / 非 GUI fork shell」二选一抽成单一接口 `launch_userspace()`(desktop_launch.cpp / shell_launch.cpp,CMake 选编),消 §14 头号反例。代码逻辑与批1 内联等价,单核 `run-kernel-test` 931/0 绿。但 `make run`(**双核 -smp 2**)**每次必 panic**。

## 诊断(对照实验坐实)

| 实验 | 结果 |
|------|------|
| `git stash` 退回批1(内联)→ `make run -smp 2` | **panic count 0(干净)** |
| 批2(launch_userspace 抽函数)→ `make run -smp 2` | **每次 panic** |
| 批2 + gui_worker 改空死循环(只 yield)→ `make run -smp 2` | **照样 panic** |

→ 元凶不是 gui_worker 内容,是**任务存在 + SMP 并发**。批2 多一层跨 TU 函数调用让时序差几时钟,踩中潜伏竞态窗口。

加 `[DIAG]` 打印 `schedule()` context_switch 前(cpu/prev_tid/next_tid/next_rip),铁证:
```
cpu1 tid3(kernel_init)->tid5(gui_worker)   gui_worker 在 cpu1 跑
cpu1 tid5->tid3                              gui_worker 让出 cpu1(保存其 ctx)
cpu0 tid4->tid5                              cpu0 同时切入 gui_worker(恢复其 ctx) ← 并发窗口
```

## 根因

任务跨 CPU 迁移时,**旧 CPU 的 `context_switch`(保存 task->ctx)和新 CPU 的 `context_switch`(恢复同一 task->ctx)并发**——同一 ctx 字段被并发读写 → 写花 → 恢复后 `ret` 跳垃圾 rip(实测 `0x1B` / `0xffffffff8002018e`)→ #UD/#BP。

- runqueue 有锁(`lock_.irq_guard()`)——**不是 runqueue 竞态**;坏的是 task ctx 字段本身。
- F4-M4 的「pick removes」挡住了两 CPU 同时**运行**同一 task,但**漏了迁移窗口**:旧 CPU 还在 save、新 CPU 已 restore。
- panic RIP `0xffffffff8002018e` 无符号(big_kernel 在 `0xffffffff81...`),证实是 ctx 写花后跳到的垃圾地址。

## 修复(对齐 Linux `task_struct->on_cpu`)

| 改动 | 文件 |
|------|------|
| Task 加 `on_cpu`(int, -1=已存/未运行, cpu_id=运行中) | process.hpp(+ static_assert 锁 ctx@0、on_cpu@96) |
| `context_switch.S` 存完 from 后设 `from->on_cpu=-1`(x86 store release;ctx@Task+0 → rdi=Task*) | context_switch.S |
| `RoundRobin::pick_next` 跳过 `on_cpu≠-1 且 ≠本cpu` 的 task(别的 CPU 正存其 ctx);**本 cpu 的不跳**(单核 yield `next==prev return` 语义保持) | roundrobin.cpp |
| `schedule`/`exit_current`/`run_first`:切入 next 前设 `next->on_cpu=本cpu`(claim) | scheduler.cpp |
| `TaskBuilder.build`/`run_first`/`setup_ap_idle` 初始化点 | task_builder.cpp / scheduler.cpp |

**为何用 pick 跳过而非同步 spin**:两 CPU 互 spin 等对方存完会死锁(A 等 B 的 task、B 等 A 的 task,都不推进自己的 context_switch)。pick 跳过(选别的或 idle)无死锁,task 留 queue 等 ctx 存完(on_cpu→-1)后下次 pick 选它——可调试优先于性能(hobby os)。

## 验证矩阵

- 单核 `run-kernel-test`:**931/0** ALL PASSED(on_cpu 不破坏单核:本 cpu task 不跳过 → yield `next==prev` return)
- `make run -smp 2` **连续 3 次 panic count 0**(AP1 online + xHCI keyboard armed + USB input primary)
- 文件行数:scheduler.cpp 500(精简 on_cpu 注释回 ≤500)、其余 < 500
- LOCKDEP -smp 2:on_cpu 用 `__atomic`(非 Spinlock,不进 lockdep 锁序图),pick_next 在既有 `lock_.irq_guard` 内(不新增锁)——无锁改动,非必须;build-lockdep 缺 `mini_kernel_test` target(配置问题,非本改动)未跑

## GOTCHA / 教训

- **context_switch 返回语义**:context_switch 的"返回"是任务被切回时(在切回任务的栈),不是"存完 prev"。所以"标记 prev 存完"只能在 asm(context_switch.S 存完 from 后立即设),C 层做不到。
- **pick removes 不够**:F4-M4 的多核安全假设"task 不在 queue 时不会被双 pick",但迁移窗口(旧 CPU save 中)需要额外同步(on_cpu)。开新弧前审 SMP 路径要看迁移窗口,不只看 runqueue 锁。
- **Heisenbug 时序**:批1 内联时序恰好避开竞态,批2 抽函数多一层调用必现。根因一直在(F4-M4 后潜伏),对应"偶现崩溃"痛点——不修以后别的改动还会踩。

## 下一步

SMP 修复在 `feat/smp-migration-fix`(1 commit `68b1913`),待 push/PR 合 main。合 main 后回 `feat/gui-decouple` rebase + pop 批2 + 验证 `-smp 2` 绿 → commit 批2,继续批3/批4。
