# F5-M5 usb-tablet 绝对鼠标(光标同步)+ 栈大户挪堆 — 2026-06-24

> 分支 `feat/f5-m5-xhci-3`,接 [2026-06-24-f5-m5-smp-shell-lstar-fix.md](2026-06-24-f5-m5-smp-shell-lstar-fix.md)(LSTAR+LAPIC timer)之后。两件事:① 换 USB **绝对指点设备**(usb-tablet)根治「QEMU 宿主光标与 Cinux 光标不重叠」;② 修tablet 测试中暴露的**内核栈溢出**——多处 PATH_MAX/Rect 大缓冲本该在堆上却在栈上,挪堆。
>
> **TL;DR**:① QEMU `usb-mouse`(相对)→ `usb-tablet`(绝对),新增 UsbTablet 驱动解 `[buttons][X16LE][Y16LE]`(0..0x7FFF)→ `Mouse::inject_usb_absolute` 直接设绝对光标 → Cinux 光标 = 宿主光标,无边缘漂移。② 路径 syscall 的 `char resolved[PATH_MAX]`+`parent_buf[PATH_MAX]`+path_canonicalize 的 `out[PATH_MAX]`(一次解析 8~12KB 栈)+ GUI 的 pump `Rect[64]`/Region::subtract `Rect[128]`,全部挪堆(RAII PathBuf / new[]);帧从 4-8KB 降到 <200B。

## Part 1 — usb-tablet 绝对鼠标:光标同步

### 现象

换 USB 鼠标后,**QEMU 宿主光标与 Cinux 画的光标位置不重叠**(移动 delta 对齐,绝对位置偏移),点不准。

### 根因

QEMU 配的是 `-device usb-mouse`——**相对指点设备**(报 dx/dy)。Cinux 自画光标(累加 dx/dy + 边缘 clamp)+ QEMU 宿主光标 = **两个独立光标**;宿主光标撞 QEMU 窗口边后 delta 丢失、宿主鼠标加速 → 绝对位置发散。VM 里相对鼠标的经典「双光标」问题,非 Cinux 坐标 bug(那是恒定偏移,会一直偏;这是「有时候」,撞边后累积)。

### 修法

切到**绝对指点设备** `usb-tablet`(报绝对 X/Y),Cinux 光标直接设到该位置 → 两光标合一:
- **QEMU**([qemu.cmake](../../cmake/qemu.cmake)):`usb-mouse` → `usb-tablet`(run + run-kernel-test-xhci)。
- **UsbTablet 驱动**([usb_tablet.hpp](../../kernel/drivers/mouse/usb_tablet.hpp)/[.cpp](../../kernel/drivers/mouse/usb_tablet.cpp)):并行于 UsbMouse,`on_transfer_complete` 解 QEMU tablet 报告 → `Mouse::inject_usb_absolute`。**不发 SET_PROTOCOL(boot)**(tablet 无相对 boot 模式,默认 report/absolute)。
- **解码**([mouse/hid.hpp](../../kernel/drivers/mouse/hid.hpp)):`decode_tablet` —— `[buttons][X16LE][Y16LE]`,X/Y 0..0x7FFF(32767)。
- **Mouse 绝对 API**([mouse.hpp](../../kernel/drivers/mouse/mouse.hpp)/[.cpp](../../kernel/drivers/mouse/mouse.cpp)):`inject_usb_absolute(tx,ty,buttons)` 把 0..32767 线性映到屏幕像素 → `update_absolute`(从 `apply_motion` 抽出的共享核心:设绝对光标+入队)。
- **枚举**([usb_init.cpp](../../kernel/drivers/usb/usb_init.cpp)):指点设备接 UsbTablet(find_boot_mouse 仍匹配——QEMU tablet 也呈现 3/1/2;保留 UsbMouse 给 host 测试)。

### 验证

光标**精确跟随宿主光标**(用户确认);run-kernel-test 931/0。

## Part 2 — 栈大户挪堆(修 tablet 测试暴露的崩溃)

### 现象

tablet 光标跟随 OK,但 shell 起来后移动鼠标触发 **#DF + FrameBuffer 花屏**。`RSP` 落某任务内核栈底(用了 ~12KB)→ 撞 guard → #PF→#DF;花屏是 panic 时 composite 帧画了一半。

### 诊断(-fstack-usage 量化各帧)

`-fstack-usage` 编译看 .su,栈大户一目了然:

| 函数 | 改前帧 | 改后帧 | 栈缓冲 |
|---|---|---|---|
| sys_rmdir/creat/mkdir/unlink | **~8KB** | <150B | `resolved[PATH_MAX]`+`parent_buf[PATH_MAX]` |
| sys_open/stat/chdir/dmesg | ~4KB | <200B | `resolved[PATH_MAX]` |
| path_canonicalize | **4KB** | 小 | `out[PATH_MAX]` |
| pump | 1168B | 160B | `Rect rects[64]` |
| Region::subtract | 2112B | 64B | `Rect staged[128]` |

**根因**:路径解析 syscall 把 `char [PATH_MAX]`(PATH_MAX=4096)直接放栈上——一次解析 `resolved`(4KB)+ `parent_buf`(4KB)+ canonicalize 的 `out`(4KB)= 8~12KB → 撑爆 16KB 内核栈(shell 的 open/stat)。这是「分明该在堆上却在栈上」的东西——Linux 只给 8KB 栈就是不这么干。

### 修法(全部挪堆)

- **PathBuf RAII**([path.hpp](../../kernel/fs/path.hpp)):`new char[PATH_MAX]`,scope 退出 `delete[]`,隐式转 `char*`。path_canonicalize + 7 个 syscall(sys_open/stat/chdir/unlink/mkdir/rmdir/creat)的 `resolved`/`parent_buf`/`out` 全改 PathBuf。
- **Cinux-GUI submodule**(都是咱仓库):pump `Rect rects[64]` → `new Rect[64]`;Region::subtract `Rect staged[128]` → `new Rect[128]`(单出口 `delete[]`)。

### 验证

- 帧暴跌:sys_rmdir 8320→144、sys_open 4176→96、pump 1168→160、Region::subtract 2112→64。
- run-kernel-test **931/0**;无 #DF/花屏(栈用量的结构性消除)。

## GOTCHA 登记候选

- **GOTCHA(PATH_MAX 上栈)**:路径 syscall 把 `char [PATH_MAX]`(4096)放栈上,两三个叠起来 8~12KB → 撑爆 16KB 内核栈。教训:内核栈(≤16KB,对标 Linux 8KB)严禁放大缓冲;PATH_MAX/大数组一律堆(RAII)或调用方传入。-fstack-usage 是定位栈户的利器。
- **GOTCHA(usb-mouse 双光标)**:VM 里相对鼠标(usb-mouse)宿主光标与 guest 光标绝对位置发散(边缘 clamp/加速)。用绝对指点设备(usb-tablet)根治。QEMU tablet 呈现 3/1/2(同 boot mouse)但报告是绝对,须按绝对解(非 boot dx/dy)。

## 改动文件

**CinuxOS**: `cmake/qemu.cmake`(usb-tablet)、`kernel/drivers/mouse/{usb_tablet.hpp,usb_tablet.cpp}`、`kernel/drivers/mouse/{hid.hpp,mouse.hpp,mouse.cpp}`、`kernel/drivers/usb/usb_init.cpp`、`kernel/drivers/CMakeLists.txt`、`kernel/fs/{path.hpp,path.cpp}`、`kernel/syscall/sys_{open,stat,chdir,unlink,mkdir,rmdir,creat}.cpp`(+ Cinux-GUI submodule 指针)。
**Cinux-GUI submodule**: `core/{pump.cpp,region.cpp}`(pump/Region 栈缓冲挪堆)。

## Follow-up

### ⚠️ 待重构:大幅度拖拽窗口卡顿(下个对话处理)

tablet 光标流畅,但**大幅度拖拽窗口卡顿**。已诊断(留作重构依据):

- `composite()`([window_manager.cpp:183](../../kernel/gui/window_manager.cpp))每帧**全屏 clear + 重绘所有窗口**到 back buffer(缓存 RAM,**快**——所以光标移动流畅)。
- 拖拽走 `invalidate_all()`([window_manager_input.cpp:98](../../kernel/gui/window_manager_input.cpp#L98))→ flush 把**整屏(1024×768×4 ≈ 3MB)**从 back buffer 写到 framebuffer。framebuffer 是 **uncached MMIO(慢)**→ 每个拖拽帧全屏 flush = 卡顿。光标只 flush 小区域所以流畅。
- **关键结论:瓶颈是大区 flush(uncached framebuffer 写),不是 composite(全屏但缓存,快)**。

**尝试过又撤回**:拖拽只失效「窗口旧+新 bounds」(免整屏 flush)→ **出现残影**(失效不全:疑漏了 title bar 的 `total_height()`;用了 `height()`=内容高),且**没缓解卡顿**(说明光减小 flush 不够,或 composite 全屏 clear 也是成本,或 gui_worker 帧率受限)。已还原 `invalidate_all()`(保正确)。

**重构方向**(下个对话):
1. `composite()` 改 **dirty 区裁剪**:只 clear+redraw dirty 区,不全屏(需让 clear/blit 支持 clip rect;`draw_desktop_icons`/`blit_to`/`draw_cursor` 都要按 dirty 区裁剪)。
2. 或优化**大区 flush**(framebuffer 写):批量/缓存友好写,或 framebuffer 映射改 cached(若硬件允许)。
3. 先**量清楚瓶颈**:加计数器测 composite 耗时 vs flush 耗时 vs gui_worker 帧率,确认是 composite 全屏还是 flush uncached,再对症。
4. 注意 Window 屏上范围用 `total_height()`(含 title bar),`height()` 只是内容高。

### 其它

- 其余 syscall(sys_dmesg 4KB 帧等)若也有大栈缓冲,后续同法挪堆(本批修了路径相关的崩点;dmesg 等非路径、未崩)。
- LAPIC timer 在 composite 深栈里嵌套——栈挪堆后余量充足;若后续仍紧,可让 timer ISR 更轻(不内联 schedule)。
