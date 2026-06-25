# F-CLN 批3 — DEBT-018 kMaxCpus 同名不一致 — 2026-06-25

> DEBT-018（kMaxCpus 两处同名不同值不同类型）闭环。

## 语义区分
两处 kMaxCpus 本是不同语义：
- `cinux::proc::kMaxCpus = 8`（[percpu.hpp:28](../../kernel/proc/percpu.hpp#L28)）：**运行 CPU 上限**，percpu_blocks/idle_tasks_/gdt_blocks/lockdep g_held 数组容量。实际 -smp 2，留余量 8。
- `cinux::drivers::acpi::kMaxCpus = 16`（[acpi.hpp:150](../../kernel/drivers/acpi/acpi.hpp#L150)）：**MADT 记录容量**，cpu_apic_ids[16] 存 ACPI 报的 LAPIC，可能 >运行上限。

不同 namespace（proc vs drivers::acpi）不撞 ODR，但同名易混。

## 修复
- acpi 的 `kMaxCpus` → **`kMaxAcpiLapics`**（区分语义：ACPI 记录容量 ≠ 运行上限）。更新 [acpi.hpp:150](../../kernel/drivers/acpi/acpi.hpp#L150)(定义 + 注释) + `:160`(cpu_apic_ids[]) + [madt.cpp:57](../../kernel/drivers/acpi/madt.cpp#L57)(cpu_count 比较)。
- proc::kMaxCpus 保留（运行上限，单一权威；ap_main:220/304 用它限 AP 启动 ≤8）。
- [ap_main.cpp](../../kernel/arch/x86_64/ap_main.cpp) 加 `static_assert(proc::kMaxCpus <= acpi::kMaxAcpiLapics)`（ACPI 记录容量须 ≥ 运行上限，否则 >kMaxAcpiLapics-CPU 硬件拓扑静默截断）。

## 验证
- big_kernel 编译零 error（static_assert 过：8 ≤ 16）+ 改名编译过
- run-kernel-test **931/0** 绿
- **-smp 2** ALL PASSED（DEBT-018 SMP 相关，回归必做）

## 产出
- acpi.hpp（kMaxAcpiLapics 改名 + 注释）+ madt.cpp（引用）+ ap_main.cpp（static_assert）+ debt.md DEBT-018 ✅ + PLAN 批3 ✅

下个：批4 DEBT-008 signal_setup_frame 校验栈 VMA（中风险，信号路径）。
