# F2-M4: Page Cache

> 日期 2026-06-17 · 里程碑 F2-M4 · 分支 `feat/f2-m4-page-cache`
> 内核级 Page Cache，让 file-backed mmap 的 demand paging 读到真文件内容。

## 背景

M2 的 mmap 文件映射只记 `backing` inode（[sys_mmap.cpp](../ai/../../kernel/syscall/sys_mmap.cpp) 批4），PF 时一律映射零页（[exception_handlers.cpp](../ai/../../kernel/arch/x86_64/exception_handlers.cpp) handle_pf）——文件映射实际读到全零。M4 补这个洞：缓存文件内容页，PF 时按 `(Inode*, page_offset)` 取缓存页映射。

## 设计

- **PageCache 数据结构**：256-bucket hash 表，侵入式双向链表，键 `(Inode*, page_offset)`。`CachedPage`（phys/virt/inode/offset/ref_count/valid + hash 链）。无淘汰（LRU 留后续）。
- **direct-map 复用**：缓存页 virt = `phys + KERNEL_VMA`（GOTCHA #7 同 M3 DmaPool），免 temp-map、不 unmap。
- **`get_page` 锁外读 / 锁内 insert**（关键，IF=0 安全）：① 锁内查 → 命中 bump ref 返；② 未命中：锁外 alloc 页 + `inode->ops->read` 填充（AHCI 轮询 IF=0 成立）+ EOF 零填；③ 锁内短临界区 insert（带 race 再查）。杜绝 IO-under-lock 重入死锁。
- **`handle_pf` 文件感知**：M1 批4 的 VMA `find()` 后，若 `vma->backing != nullptr && !Anonymous`，算 `file_offset = vma->file_offset + (fault_addr - vma->start)`，`get_page` → `map_nolock` 映射；PTE 权限按 VmaFlags 翻译（Write→WRITABLE、!Exec→NX，原匿名路径硬写 WRITABLE 不变）。匿名/无VMA 路径字节不变；文件路径失败回退匿名零页（不挂）。
- **`sys_mmap` offset 页对齐校验**：文件映射要求 offset 页对齐（cache 按页键），否则 EINVAL。

## 关键决策

1. **最小 MVP（读路径）**：只做 cache + file-mmap demand-read。脏页写回 / MAP_SHARED 写一致性 / 全 `read()` 经缓存 / LRU+跨进程共享+CoW 全留后续（M6 ext2 Cache / F3 / M5）。
2. **direct-map 复用缓存页 virt**（非 todo 草案的 `map_temporary`）：和 DmaPool 同款，免临时映射槽管理。
3. **`get_page` 返回 `ErrorOr<CachedPage*>`**（A.6，新代码无 legacy）。
4. **execve ELF 段是匿名 VMA**（不设 backing，内容 eager 加载）——故 boot 走匿名路径，文件 PF 路径 boot 期间 dormant，不会因文件路径炸 boot。

## 陷阱

- **GOTCHA #10 NXE 未启用 → NX 保留位**：F9 未做，EFER.NXE 关闭，PTE 设 `FLAG_NX`(bit63) 触发 reserved-bit #PF（err=0x8）循环。handle_pf 文件路径初版给非 exec 页设 NX → Test B PF round-trip 无限循环。诊断：PF handler 临时打印 err/cr3/asPml4/translate，err=0x8(RSVD)+translate==phys 确认 PTE 设对、保留位违例。修复：handle_pf 文件路径 + mprotect 都暂不设 NX（留 F9 NXE 启用后）。
- **单测验 handle_pf 文件 PF 需 `as.activate()`**：big_kernel_test 跑 boot PML4（user 半空），必须 `as.activate()` 切到进程 PML4（内核半区镜像故继续跑）再访存，PF handler 才映射进正确页表；用完 `write_cr3(kernel_pml4())` 恢复（as 析构前）。
- **IF=0 下 `get_page` 的 IO**：ext2 `read_block` → AHCI 轮询，IF=0 单核成立；读目标是 direct-map 页（present），不触发重入 PF。`new CachedPage` 走 heap（预映射 64KB headroom 内安全；堆耗尽需增长则可能 PF——MVP 缓存小不触）。
- **缓存页是共享的**：MVP 无 CoW，MAP_SHARED 文件写会改共享缓存页（不回盘）——已知限制，测试用 PROT_READ 规避。

## 验证

- run-kernel-test：722→730（+8：批1 page_cache 6 测 [命中/未命中/refcount/二次命中/EOF零填/release] + 批3 file_mmap 2 测 [Test A cache 真盘读 ext2 字节比对+cache hit；Test B 文件 VMA PF round-trip 端到端]），批2 回归（数不变）。全绿。
- 全量 build（含 host test_fork_exec）：CI 对等（未改公共接口/InodeOps/mock）。
- 实机冒烟：跳过——文件 PF 路径 boot dormant + run-kernel-test 已带改动启动内核；GUI headless 启动对 M4 边际价值低。

## 遗留

- **NX 强制（非 exec 页）**：待 F9 启用 EFER.NXE 后，handle_pf 文件路径 + mprotect 再开 FLAG_NX（现暂不设，避免保留位 fault）。
- 脏页写回 / MAP_SHARED 写一致性 / 全 `read()` 经缓存（→M6 ext2 Cache）/ LRU 淘汰 / 跨进程共享缓存 + CoW-for-shared-file（→F3/M5）。
- user libc mmap wrapper（F10）：让 user 程序直接 mmap 文件（当前 Test B 经 ring-0 + as.activate 模拟）。
