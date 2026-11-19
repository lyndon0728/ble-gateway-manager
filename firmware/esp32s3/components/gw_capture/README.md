# 采集、持久接管与平台上报

本组件实现 ESP32-S3 对 nRF54L15 批次的持久接管、本机 BLE 接入、双射频配对、HTTP 原始事件上传和独立 Peripheral inbox。接口见 [gw_capture.h](include/gw_capture.h)，线协议依照 [UART 协议](../../../../server/docs/gateway-uart-protocol.md)，云端 JSON 依照 [平台协议](../../../../server/docs/gateway-protocol.md)。

## 存储布局与容量

使用 `gwspool`、data subtype `0x40`、大小恰好 1 MiB 的原始分区。生产配置 `CONFIG_GW_REQUIRE_FLASH_ENCRYPTION=y` 时，同时检查分区 encrypted 标记和芯片实际 Flash 加密状态。读取业务数据走解密 API；判断新空盘使用 `esp_partition_read_raw`，避免把加密分区的擦除态误读为普通明文全 FF。

| 区域 | 大小 | 用途 |
| --- | --- | --- |
| `0x00000..0x1FFFF` | 128 KiB | 16 个轮转根快照，每个 8 KiB |
| `0x20000..0xFFFFF` | 896 KiB | 224 个轮转数据槽，每个 4 KiB |

**可同时保存约 224 条待确认观测/事件，不是按 payload 字节紧密装填的 896 KiB 队列。** 即使一条广播仅 31 字节，也占一个 4 KiB 槽。待配对记录、待上传事件、永久拒绝隔离记录和 Peripheral inbox 共用这个硬上限。配对成功后释放 ESP 观察槽，保留 nRF 主记录槽。Peripheral 另有最多 32 条、原始应用字节最多 16 KiB 的逻辑配额，不能无界占满整个分区。

`gw_capture_used_bytes()` 返回当前被观测/事件占用的数据槽字节数；不把 128 KiB 元数据保留区计为待上传业务。诊断同时报告 `capacityRecords=224`、pending、ready、quarantined、peripheralInbox 和 storageFault。

## 原子提交和恢复

持久格式为显式小端整数，不直接写入 C 结构体。数据槽有格式标记、源控制器启动 ID、采集/接收/时间映射快照、完整原始记录、入队次序和 CRC32C。根快照保存槽状态、不可变事件 ID 的启动 ID/序号、配对标记及最新 nRF 批次水位。水位含 boot、batch、first sequence、count、type 和原始 payload 的 SHA-256。

一次 nRF 提交依次执行：

1. 校验整个批次及水位连续性，预留足够空槽；原数据不足空间时返回 BUSY。
2. 擦除下一个轮转根，保留当前已提交根。
3. 擦除并写入当前根未引用的空槽，逐槽读回校验。
4. 写入新根的完整内容，最后单独写 32 字节提交尾部；提交标记位于尾部最后 8 字节。
5. 读回验证新根，发布新的根索引；至此才能返回 `CUSTODY`。

整个批次的所有观测和最新水位由同一个根发布。接收成功但根未提交时，恢复后不承认接管；新根已经提交但应答丢失时，重放根据水位返回 CUSTODY。最近批次完全一致才重放接管；同序号不同内容、跳号或结构错误进入 FAULT；落后批次返回回调值 `255`，UART 层不发 ACK。

根和数据槽均轮转写入，未交付的数据不能覆盖。回收业务槽不删除最新 nRF 水位。旧根在擦除到一半时可能仍留旧提交尾部，恢复只在存在更新的完整根时忽略这种旧状态；比最新有效根更新的完整提交尾部内容损坏时，停止恢复并报 FAULT，不能回滚接管承诺。

只有整个分区的原始读取均为擦除态，才自动建立首个空根。没有有效根但残留任意非擦除字节时停止，不清空分区。首次格式化本身断电也可能需要维修工具核查后重新初始化；正常启动从不自动格式化故障盘。

当前验证的掉电模型是读/擦/写操作之前、部分完成或完成后发生中断。CRC 用于发现存储损坏，不提供抗物理篡改认证。16 份元数据轮转降低固定热点，仍需针对实际 Flash、扫描峰值、批次大小和供电执行写入寿命、吞吐与欠压测试；主机测试不能代替这些实板结果。

## 两路扫描与时间

本机入口 `gw_capture_esp(record, size)` 使用 UART BLE 的同一个 24 字节记录头，`capture_ms` 为 ESP 单调毫秒。返回 ESP_OK 代表已持久接管；满或内存不足返回 ESP_ERR_NO_MEM，调用者只丢弃新观测并计数。不要从 NimBLE 控制器回调直接执行 Flash 写入，应通过有界 worker 队列调用。

两路是否启用由 `gw_capture_radio_enabled(0, enabled)` 与 `gw_capture_radio_enabled(1, enabled)` 更新。只有两路同时扫描时等待配对。比较地址类型/地址、ADV 或 SCAN_RSP、legacy/extended、扩展 SID、ADV connectable 及全部 AD 字节；先用 SHA-256 找候选，再逐字节比较。未知地址、缺失扩展 SID、时间不可比较的记录独立保留。同一路的周期性广播不会互相消除。

定稿 worker 每 10 ms 检查 150 ms 等待期限，到期选当前时间距离最近的另一射频观测，同差按入队顺序；到达时间晚于该观测截止期限的记录不能补配。时间差加两端误差不超过 50 ms，才能一对一合并。实际调度和 Flash 写入时延须实板验证，150 ms 是逻辑截止时刻。默认暂存原始记录字节上限 128 KiB；下一批将超过上限时，先将已有暂存记录单独定稿。物理记录容量先达到上限时返回 BUSY。

匹配对以 nRF 为主，在一次根提交中生成一个平台事件、固定其身份和 observedBy，同时消费两条观测。重启恢复时先将上一 ESP 启动周期的暂存记录分别定稿，不使用过期单调映射重新配对。事件 ID 在定稿时分配；根中持久保存启动 ID 和单调序号，HTTP 重试不分配新 ID。

独立时钟 worker 在 nRF boot 改变时及每 30 秒发送 TIME_PROBE。`gw_nrf_command(0x04)` 必须只发一次；响应校验成功、两端时间非倒退、扣除 nRF 处理时间的 RTT 不超过 50 ms 才采纳。中点映射最多用于距锚 60 秒的采集；误差包括半 RTT、毫秒量化及保守的 100 ppm 相对漂移上界。该漂移假设也需实板时钟测量确认。可信 ESP UTC 才输出 SYNCED 和 capturedAt；无可信 UTC、省略时间；UTC 跳变后重建时间锚。已经持久化的事件时间保持不变。

## 上报与逐项接管

上传固定使用 IngressBatch 1.1，HTTP 路径为 `POST /api/v1/gateway-ingress/batches`。TLS、设备认证和响应上限由 gw_http 负责。单次最多 64 个事件，先上传较早事件，保留原始 AD 或 GATT value 的 Base64；没有可信时间时使用 UNSYNCED 并省略 capturedAt。

同时匹配 schemaVersion、namespaceId、gatewayId、batchId 和 `durability=RAW_PERSISTED`，才考虑 ACK 的 items。每个发送 eventId 必须恰好出现一次：

| 单项结果 | 处理 |
| --- | --- |
| ACCEPTED / DUPLICATE | 原子更新根后释放该槽 |
| REJECTED_PERMANENT | 原始数据留在隔离槽，不紧密重发 |
| REJECTED_RETRYABLE | 保留，遵守 retryAfterMs |
| 缺项、重复项、未知状态 | 保留该事件 |
| 请求超时、丢失响应、错误身份或错误 batchId | 所有受影响事件保留，执行指数退避与抖动 |

一次 ACK 可以同时回收部分事件、隔离部分事件、保留其他事件。传输成功或 HTTP 返回 200 本身不是接管。隔离数据不自动清空；达到物理容量会反压 nRF 和本机采集。

当前编解码格式固定为 spool v3 / RawEvent 1.1。后续改变生成语义必须保留旧格式读取与旧 JSON 映射，或在维护窗口确认旧 spool 排空后迁移；不能升级后给同一个 eventId 生成不同内容。

## GATT、连接证据与 Peripheral

`gw_capture_connection_ref` 按协议计算 SHA-256(`gw32-conn-v1` ASCII + radio 单字节 + boot 16 字节 + conn_id 小端 4 字节)，取前 16 字节小写十六进制。ESP GATT 使用 `gw_capture_esp_boot` 的启动 ID，以及 `gw_capture_esp_gatt` 的 24 字节 GATT 记录头接入。nRF 通知的连接 ID、地址和句柄来自记录本身，连接结束后仍保留原快照。

`gw_capture_adv_evidence` 只接受实际定稿的 NRF54 可连接 ADV、精确 eventId、当前 nRF boot、地址/类型和可信采集时间。最近 64 个可用证据在 RAM 缓存；已 ACK 数据释放后仍可在本次 ESP 运行中验证，重启后无法恢复的已回收证据保守拒绝。它不以收到命令的时间替代原始采集时间。

`gw_capture_link_down` 的调用没有可信断开采集时刻，因此链路结束事件固定为 UNSYNCED、省略 capturedAt 和可选 sequence。事件 ID 使用 `link-<connectionRef>-<reason>`；同一事实重复调用、已交付后再次提交，都生成完全相同的 ID/内容，依靠原始事件幂等接管，不制造新的链路事实。仅使用 `GATT_LINK_DOWN`、connectionRef、linkReason 和空 payload，不混入地址等 schema 不允许的字段。此处是针对没有采集时间的内部连接状态事件的明确映射规则。

UART `0x22 PERIPHERAL_BATCH` 只进入独立 inbox 状态，不能进入 RawEvent、GATT_NOTIFY 或配置/升级执行器。`gw_capture_peripheral_peek()` 返回标明 UNAUTHENTICATED 的不透明字节、连接引用和稳定 receiptToken。应用认证、防重放、授权和独立持久接管完成后，应用层才能调用 `gw_capture_peripheral_ack(token)` 释放数据；目前没有自动执行该确认的云端路由。BLE 连接本身不授予管理权限。

## 主机验证

纯 C 测试直接编译生产 `gw_spool.c`、`gw_capture_rules.c` 和共享 `gw_wire` 实现。适配器测试直接编译生产 `gw_capture.c`，使用真实 cJSON / Mbed TLS；只替换分区、时钟、FreeRTOS 与 HTTP I/O。测试生成的 JSON 再通过仓库真实 message-contracts schema 校验。

```powershell
# 仓库根目录；ESP-IDF 源码须包含 cJSON 和 Mbed TLS 子模块。
python -m pip install 'jsonschema>=4.22,<5'
python firmware/esp32s3/components/gw_capture/tests/run_host_tests.py `
  --cc '<C11 compiler or zig executable>' --idf '<ESP-IDF v5.5.6 directory>'
```

已覆盖批次重放/过期/冲突、水位回收后保持、满环不覆盖、槽复用、69 个 I/O 断电点、根/数据损坏、隔离/inbox、双路一对一判定、时间误差与锚有效期、稳定 JSON 重试、混合逐项 ACK、缺失/重复/错误身份 ACK、退避、Peripheral 不误报及幂等 link-down。五份生产路径生成的 ADV、SCAN_RSP、GATT、link-down 与混合批次通过 IngressBatch 1.1 schema。

目标芯片编译与实板状态以 [总验证记录](../../VALIDATION.md) 为准。以上测试不证明真实 Flash 加密熔丝状态、UART 高负载、射频时间误差、实际掉电波形或设备到服务器的实网闭环已经验收。
