# 2026-06-23 F5-M5 批3A — USB SETUP 包 + 描述符 + xHCI context 编码器（纯协议层,host 测）

## 背景 / 目标

**Phase 3 第一批,协议地基。** Phase 0-2（PR#30）让 xHCI 控制器跑起来（929/0,命令管线+中断端到通）但还没枚举任何 USB 设备。本批把 3B（Address Device 状态机）/ 3C（GET_DESCRIPTOR+SET_CONFIGURATION）/ 4A（HID boot）要消费的**纯编码层**先做出来 + host 测。完全沿用 1B/2A「纯 header + host 单测」范式:**无硬件交互,无内核行为变化**。

**范围收敛（对计划原文的精修）**：计划 3A 写「slot/context + 控制传输(SETUP/Data/Status)」。其中两块有「不能猜 spec」风险,处理如下——

1. **xHCI context 位布局**：从 Linux `drivers/usb/host/xhci.h` **逐位核实**（ROUTE_STRING/DEV_SPEED/LAST_CTX/EP_TYPE/MAX_PACKET/TR_DEQ_PTR_MASK 等宏）→ 本批做成编码器。
2. **控制传输 stage TRB 的 TRT 方向语义**：Linux 宏 `TRB_DATA_OUT=2 / TRB_DATA_IN=3` 与 spec 记忆(2=IN,3=OUT)相左,有歧义 → **挪到 3C**,用真实 GET_DESCRIPTOR(IN)/SET_CONFIGURATION(OUT) 对照 QEMU 设备响应验证方向,不在本批猜。

## 变更（全 header-only,3 新 header + host 测）

- **`usb_request.hpp`**（106 行）:bmRequestType 编码（dir<<7|type<<5|recipient）+ 标准请求码（kGetStatus…kSynchFrame;`kSetAddress=5` 标注「xHCI 不发,控制器用 Address Device 命令寻址」）+ `UsbSetup`(8B)+ `setup_to_u64`(8B 小端打包,喂 SETUP stage TRB parameter)+ `make_setup`/`make_get_descriptor_setup` 便捷构造。
- **`usb_descriptor.hpp`**（128 行）:描述符类型码 + 4 个 packed 结构(Device 18B/Config 9B/Interface 9B/Endpoint 7B,各 `static_assert` 尺寸)+ HID boot 常量(class 0x03/subclass 0x01/mouse 0x02/kbd 0x01)+ endpoint 助手（`ep_address`/`ep_dir_in`）。
- **`xhci_context.hpp`**（147 行）:枚举（UsbSpeed/SlotState/EpState/EpType）+ 32B 存储结构（`SlotContext`/`EndpointContext`/`InputControlContext`,各 `volatile uint32_t dw[8]` + `static_assert(sizeof==32)`）+ **纯函数编码器**（`slot_dev_info`/`slot_dev_info2`/`slot_dev_state`/`ep_info`/`ep_info2`/`ep_dequeue_ptr`/`ep_tx_info`/`input_add_flag`,返回 dword/u64,host 可测,不碰 volatile 结构）。
- **`test/unit/test_usb.cpp`**（新,117 行）:bmRequestType 位打包 / GET_DESCRIPTOR+SET_CONFIGURATION / `setup_to_u64` 小端 / 描述符尺寸 / HID 常量 / endpoint 助手 —— 10 例。
- **`test/unit/test_xhci.cpp`**（+75 行,section 5）:context 编码器位布局（slot_dev_info route/speed/last_ctx、ep_info2 cerr/ep_type/max_packet、ep_dequeue_ptr [63:4]phys+[0]DCS、input_add_flag）—— 10 例。
- **`test/CMakeLists.txt`**:`add_cinux_test(usb)`（header-only,无 link）+ ALL_HOST_TESTS 加 `test_usb`。

## 关键陷阱（GOTCHA）

- **描述符尾部对齐填充** ⚠️:`UsbConfigDescriptor`(应 9B)/`UsbEndpointDescriptor`(应 7B)因含 uint16 字段,struct 对齐=2,sizeof 被补齐到 10/8 → `static_assert` 当场抓出（**这正是 static_assert 布局锁的价值,CODING-TASTE §8**）。根因:这些是**设备 DMA 缓冲读的线缆协议结构**,wire 布局固定 9/7B,编译器按成员最大对齐(2)补尾。**修:4 个描述符统一 `__attribute__((packed))`** 强制精确布局（代码库 MADT/ICS 同款）。`UsbDeviceDescriptor`(18B,恰偶数)自然无填充但也 packed 统一意图。`UsbSetup`(8B)无此问题（偶数）,不 packed。
- **纯编码器 vs volatile 存储**:`SlotContext`/`EndpointContext` 持 DMA 共享内存（成员 volatile）→ 非 literal 类型,**不能进 constexpr**。故编码器设计成**纯函数返回 uint32_t/u64**（`slot_dev_info(...)` 等）,host 可测;内核（3B）再 `ctx.dw[0] = slot_dev_info(...)` 写入 DMA 结构。编码逻辑与存储解耦。

## 验证

- run-kernel-test:**929/0 ALL PASSED**（header-only,内核无源改动,零回归）。
- test_host:**100% passed,0 failed out of 53**（+test_usb,新 10 + 扩展 10 context 编码器例全过）。
- 全量 `cmake --build build`:绿（CI 盲区:run-kernel-test 不编 test/unit/,全量确认 test_usb 编译 + 内核零回归）。
- clang-format:5 文件过（local clang-format 22;CI format job 已禁用,见 format-ci-disabled-version-mismatch memory）。
- 行数:全 <500（max test_xhci.cpp 224）。

## 位布局核实（来源 Linux xhci.h,非猜）

- Slot DW0 dev_info:`[19:0]` Route｜`[23:20]` Speed｜`[25]` MTT｜`[26]` Hub｜`[31:27]` LastCtx
- Slot DW1 dev_info2:`[15:0]` MaxExitLatency｜`[23:16]` RootHubPort｜`[31:24]` MaxPorts
- Slot DW3 dev_state:`[7:0]` DevAddr｜`[31:27]` SlotState
- EP DW0 ep_info:`[2:0]` EPState｜`[9:8]` Mult｜`[23:16]` Interval
- EP DW1 ep_info2:`[2:1]` CErr｜`[6:3]` EPType(4=control)｜`[15:8]` MaxBurst｜`[31:16]` MaxPacket
- EP DW2/3 deq:`[63:4]` DeqPtr(16B 对齐)｜`[0]` DCS
- EP DW4 tx_info:`[15:0]` AvgTRBLen｜`[31:16]` MaxESITPayload(lo)
- InputControl DW0:A0..A31 add-flags（bit0=slot,bit1=EP0…）;DW1:D0..D31 drop-flags

## 遗留

- **3B**:Address Device 状态机 —— Enable Slot 命令 + input context 构建（消费本批编码器）+ 端口 reset（PORTSC speed/power 位,3B 实测 QEMU 补）+ Address Device 命令;`XhciSlot` DMA 类（device/input context DmaBuffer + EP0 transfer ring）。
- **3C**:控制传输 stage TRB 构建（SETUP/Data/Status 控制字 + **TRT 方向对照真实 HW 验证**）+ GET_DESCRIPTOR/SET_CONFIGURATION。

---

commit：（本次,批3A）。
