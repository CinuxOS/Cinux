# F7-M5 批3 — TCP 数据传输 + 四次挥手

> F7-M5 第三批。commit `ec0aa78`。接批2（握手 FSM）。批3 收尾 TCP 状态机（数据 + teardown）。

## 背景 / 目标

批2 立了握手。批3 在 Established 上做数据传输（序号推进 + ACK）+ 四次挥手（FIN/ACK 交换）。
完成后 TCP 核心状态机（握手/数据/挥手）全齐——批4 在内核 loopback 端到端复验 + e1000 TX smoke。

## 设计

### 数据传输（Established）

`send(dev, local_port, remote, data, len)`：发 PSH|ACK 段（seq=snd_nxt, ack=rcv_nxt），成功后 `snd_nxt += len`
（数据占 len 个序号）。仅 Established 允许。

handle 的 Established 分支：**严格按序接收**——`h.seq == rcv_nxt` 才处理（无乱序重组/重传，最小可用，
follow-up）。数据：`on_data(...)` 投递 + `rcv_nxt += data_len`；FIN：`rcv_nxt += 1` + `on_close(...)` + → CloseWait；
任一推进后回 ACK（ack=rcv_nxt）。一段可同时带数据 + FIN（一次 ACK 覆盖）。

### 四次挥手

`close()`：Established → 发 FIN（snd_nxt+1）→ **FinWait1**；CloseWait → 发 FIN → **LastAck**（响应端补刀）。

handle 的 teardown 分支（序号不变量：FIN 占 1 序号，ACK 它 = seq+1）：
- **FinWait1**：收 ACK 且 `ack==snd_nxt`（我们的 FIN 被 ack）→ **FinWait2**。
- **FinWait2**：收 FIN → 回 ACK（ack=seq+1）→ **Closed**。
- **CloseWait**：等 app 调 close()；重传的 FIN → re-ACK。
- **LastAck**：收 ACK 且 `ack==snd_nxt` → **Closed**。

loopback 上的完整 4-way（一个 TcpModule 跑双端）：client.close→FIN；server Established 收 FIN→ACK+on_close→
CloseWait；client FinWait1 收 ACK→FinWait2；server.close→FIN→LastAck；client FinWait2 收 FIN→ACK→Closed；
server LastAck 收末次 ACK→Closed。两端全 CLOSED。

### 无 TIME_WAIT

Linux 收 FIN 后进 TIME_WAIT 等 2MSD 防末次 ACK 丢。CinuxOS **无内核 timer-wake**，故省略 TIME_WAIT——
信任对方的末次 ACK 不会丢（无重传的诚实代价）。明记 follow-up：真重传 + RTO + TIME_WAIT 需 timer 基建。

## 决策

- **严格按序 + 丢乱序**：不重组、不缓存乱序段、不重传。乱序段直接丢（真栈会 re-ACK 触发重传）。
  这是最小可用——诚实标注「单方向、无重传」，不包装成可靠传输。
- **FIN 的 ACK 用 h.seq+1**：FinWait2/CloseWait 回 FIN 的 ACK 时用 `h.seq+1`（FIN 占 1 序号），
  不依赖 rcv_nxt 跟踪（这两态 rcv_nxt 可能未精确维护，直接从段算更稳）。
- **close() 双入口**：Established（主动发起）与 CloseWait（对端先关，我方补刀）都合法，分别去 FinWait1/LastAck。
- **do_handshake 复用**：数据/挥手测共用一个 helper 把双方推到 Established，避免每个测重复握手步骤。

## 陷阱

- **Established 的纯 ACK 必须 h.seq==rcv_nxt**：对端 ack 我方数据的纯 ACK 段，其 seq == 对端 snd_nxt ==
  我方 rcv_nxt（他们发到此），故过门但不触发 need_ack（data_len=0、无 FIN）——纯 ACK 正确成 no-op，
  不会无限回 ACK。
- **window 固定 8192**：无流控（滑动窗口 follow-up）；每段通告固定窗口，对端总有发送余地。
- **loopback 单 poll 多包**（批4 用）：握手 3 + 数据 1-2 + 挥手 4 全靠 `poll()` budget loop 排干；
  本批 host 测用 NoL2Dev 手动逐步喂回（确定性逐步断言），批4 才用真 LoopbackDevice 单 poll。

## 验证

- host `test_net_tcp`：**9/0**（+2：established 数据 round-trip + ACK 推进 rcv_nxt / 4-way 挥手双方 CLOSED）。
- `run-kernel-test-all` 两 leg 各 **986/0**（`ALL TESTS PASSED` ×2），零回归。

## 下一步

批4：内核 loopback 端到端（`test_net.cpp::test_tcp_loopback`，单 poll 排干握手+数据+挥手）+ e1000 TX smoke
（`test_tcp_e1000_tx`，发 TCP 段 ARP resolve + send ok）。批5 note + ROADMAP ✅。
