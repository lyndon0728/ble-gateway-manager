# UART、双射频、Beacon 过滤和 GATT 人工测试交接

基线日期：2026-11-19；适用源码：本仓库 ESP/nRF 0.3.0、GW32-E2E-1.1、UART 文档1.2（线上帧 version=1）、BFLT-2.0。**本文件仅交接测试方法，所有用例初始状态均为「未执行」。没有在本轮连接串口、发射射频、调用平台、烧录或执行故障注入。**

## 1. 开始前的阻塞条件与记录

1. nRF 尚无完整 NCS v3.2.1 目标 `west build/sysbuild` 和实板验收证据。先由构建负责人提供与实际板修订一致的产物、配置、SHA-256、链接内存报告及烧录记录。缺少时，涉及 nRF 实物的步骤记为 **BLOCKED：目标构建/烧录证据缺失**，不能用已通过的 host 测试代替。
2. 自定义板默认 `CONFIG_GW_RADIO_VERIFIED=n`；ESP 的 `CONFIG_GW_NRF_RESET_BOARD_VERIFIED` 默认关闭。射频/FEM/时钟/电源及复位线板级验收、制造授权与实际驱动能力必须先分别放行。**本交接不要求也不授权直接把这些配置改成 y，不用 GPIO8 人工拉低绕过复位门控。** 缺少射频证据时只做允许的 UART 管理和平台离线预览，其余记阻塞。
3. 核对实际网表后连接。该板基线：ESP TX GPIO17→nRF RX P1.05；nRF TX P1.04→ESP RX GPIO18；ESP RTS GPIO15→nRF CTS P1.07；nRF RTS P1.06→ESP CTS GPIO16。电平3.3 V CMOS，1,000,000 baud、8N1、硬件 RTS/CTS，CTS低允许发送。GPIO8接nRESET，正常为高阻。其他板版本不得照抄引脚。
4. 使用专用测试网关和作用域。保存当前完整扫描/广播目标、平台 revision、固件/板版本、boot ID、能力清单、授权范围。`GET .../configuration` 的 desired/reported 是补丁，**不是完整快照**；完整平台历史快照可用 `GET /api/v1/gateways/{gatewayId}/configuration-snapshot`，同时保存设备读回和实际状态。
5. 整机模式由 ESP 独占 nRF UART；逻辑分析仪只被动采集。使用 `uart_probe.py` 主动测试时，须通过已审核的隔离接线断开 ESP 对此 UART 的驱动，并停止其管理，禁止两主机同时驱动 TX/RTS。独立 probe 不发送 CUSTODY，不能充当 ESP 持久队列。每次 CLI 调用建立新 link ID，但不复位 nRF。
6. 准备可编程 BLE 广播/GATT 对端、原始空口抓包设备、双向 UART/流控采集、设备日志、平台任务/原始事件导出。精确500 ms、50/150 ms、未知RSSI、接收队列拥塞、驱动错误及协议故障需要能控制时间戳/输入/失败点的夹具和可追溯测试构建；没有时相应子项记阻塞。不要虚构仪器菜单或用手动点击声称精确时间。手机只可作为可选标准 BLE 测试客户端，**不是扫描/Beacon/GATT验收的必需业务端**；本篇不测 C05 的手机业务认证。
7. 当前无线基线不承诺扫描+广播、扫描+Central、广播+Central并行；nRF bit0（完整扩展接收）和bit5～8（已验证组合）不声明。先单独测试每种角色，再验证明确的 BUSY/PAUSED/恢复行为，不能把拒绝不支持的组合判成漏实现。当前每个 radio 的 Central/Peripheral资源按单连接基线测试。
8. 测试变体会创建新任务并占用持久任务证据；当前任务journal有界。预先按测试批次规划网关、存储配额及已有受授权维护方案。接近64项上限时停止并记录，不用删除journal、擦Flash、改业务ID规避容量和防重证据。

每个用例建立记录：`编号/子项、日期、操作者、板号、三端commit、SDK、制品SHA、授权证据、boot/link/revision、输入文件SHA、步骤实际值、预期/实际、PASS/FAIL/BLOCKED、证据路径、恢复结果`。配置变更与抓包统一时间线；屏蔽token/密钥。即使软件预览通过，空口、UART和设备状态仍须分别填写。

## 2. 共用操作与可复制样例

### 2.1 管理 API：配置、预览、任务

以下为 **PowerShell 7**，在仓库根目录使用。操作者先设置真实 `GW_API`、`GW_GATEWAY_ID`、`GW_OPERATOR_TOKEN`；不把token写入交接记录。权限按实际作用域提供 `gateway:read`、`gateway:configure`、`task:create` 和相应 `gatt:manage/gatt:execute`。远程任务还要求设备取得有效的可信时间证据。

```powershell
$api = $env:GW_API.TrimEnd('/')
$gateway = $env:GW_GATEWAY_ID
$headers = @{ Authorization = "Bearer $env:GW_OPERATOR_TOKEN" }
$configUri = "$api/api/v1/gateways/$gateway/configuration"
$radio = 'nrf54' # 第二轮仅改为 esp32；一次任务只改一个radio、一个配置组
function Copy-Json($Value) { $Value | ConvertTo-Json -Depth 40 | ConvertFrom-Json -AsHashtable }
function Hex-B64([string]$Hex) { [Convert]::ToBase64String([Convert]::FromHexString($Hex)) }
function Send-Configuration($Configuration) {
    $r = Invoke-WebRequest -Uri $configUri -Headers $headers
    $current = $r.Content | ConvertFrom-Json
    $script:lastBody = @{
        expectedReportedRevision = $current.reportedRevision
        configuration = $Configuration
        expiresAt = [DateTime]::UtcNow.AddMinutes(10).ToString('o')
    } | ConvertTo-Json -Depth 40
    $script:lastHeaders = $headers.Clone()
    $script:lastHeaders['If-Match'] = [string]($r.Headers.ETag | Select-Object -First 1)
    $script:lastHeaders['Idempotency-Key'] = [guid]::NewGuid().ToString()
    Invoke-RestMethod -Method Put -Uri $configUri -Headers $script:lastHeaders -ContentType 'application/json' -Body $script:lastBody
}
function Send-Scan($Profile) { Send-Configuration @{radios=@{$radio=$Profile}} }
function Preview-Scan($Profile, $Samples=@()) {
    $body = @{radio=$radio;profile=$Profile;samples=@($Samples)} | ConvertTo-Json -Depth 40
    Invoke-RestMethod -Method Post -Uri "$api/api/v1/gateways/$gateway/scan-profile-preview" -Headers $headers -ContentType 'application/json' -Body $body
}
function Send-Task([string]$Type, $Parameters) {
    $script:lastTaskBody = @{gatewayId=$gateway;type=$Type;parameters=$Parameters;expiresAt=[DateTime]::UtcNow.AddMinutes(2).ToString('o')} | ConvertTo-Json -Depth 30
    $script:lastTaskHeaders = $headers.Clone()
    $script:lastTaskHeaders['Idempotency-Key'] = [guid]::NewGuid().ToString()
    Invoke-RestMethod -Method Post -Uri "$api/api/v1/tasks" -Headers $script:lastTaskHeaders -ContentType 'application/json' -Body $script:lastTaskBody
}
# 只读：保存设备当前配置记录与完整平台快照。
Invoke-RestMethod -Uri $configUri -Headers $headers
Invoke-RestMethod -Uri "$api/api/v1/gateways/$gateway/configuration-snapshot" -Headers $headers
# 提交后，从响应取实际taskId，再替换下例变量；未终结前不得提交下一个变体。
# $taskId = '实际返回值'
# Invoke-RestMethod -Uri "$api/api/v1/tasks/$taskId" -Headers $headers
```

配置真正成功须核对任务设备结果为 `SUCCEEDED`、`verification=VERIFIED`、`details.configuration` 回显请求和正确 `reportedConfigurationRevision`，并核对 `effective`、设备读回与空口。API受理、desiredRevision前移或MQTT PUBACK都不算执行成功。请求结果不明时保留相同body/幂等键重发，不重新调用生成新键的函数。状态冲突先对账，不循环覆盖。所有变体按顺序执行，失败或阻塞不继续堆任务。

### 2.2 Beacon 公共输入和配置

`AdvHex` 含 Flags+iBeacon，UUID=`fda50693-a4e2-4fb1-afcf-c6eb07647825`，Major=0x1234、Minor=3；`RspHex` 是Complete Name `Bcn-01`。C5是iBeacon校准值，不是此次接收RSSI。默认唯一ADV在t=1000 ms、RSP在t=1100 ms；ADV=-60 dBm，RSP=-90 dBm，实际地址/类型相同。

```powershell
$AdvHex = '0201061aff4c000215fda50693a4e24fb1afcfc6eb0764782512340003c5'
$RspHex = '070942636e2d3031'
$PairProfile = @'
{"scan":{"enabled":true,"active":true,"intervalMs":100,"windowMs":50},"filters":{"minRssi":null,"serviceUuids":[],"manufacturerIds":[]},"beaconRules":[],"beaconFilterV2":{"version":2,"scope":"ADV_SCAN_RSP_PAIR","pairWindowMs":500,"rules":[{"action":"ALLOW","reportKinds":["LEGACY_ADV"],"fieldMask":31,"name":{"mode":"PREFIX","source":"COMPLETE","valueBase64":"QmNuLQ==","maskBase64":"/////w=="},"ibeaconUuid":{"value":"fda50693-a4e2-4fb1-afcf-c6eb07647825","maskHex":"ffffffffffffffffffffffffffffffff"},"rssi":{"mode":"RANGE","minDbm":-75,"maxDbm":-35},"major":{"value":4608,"mask":65280},"minor":{"value":3,"mask":15}}]}}
'@ | ConvertFrom-Json -AsHashtable
function New-ReportProfile([int]$Fields) {
    $p = Copy-Json $PairProfile
    $p.beaconFilterV2.scope = 'REPORT'
    $p.beaconFilterV2.Remove('pairWindowMs')
    $r = $p.beaconFilterV2.rules[0]
    $r.fieldMask = $Fields
    $names = @('name','ibeaconUuid','rssi','major','minor','serviceUuid')
    for ($i=0; $i -lt 6; $i++) { if (($Fields -band (1 -shl $i)) -eq 0) { $r.Remove($names[$i]) } }
    $p
}
function Report-Sample([string]$Hex=$AdvHex,[int]$Rssi=-60) {
    @{reportKind='LEGACY_ADV';rssiDbm=$Rssi;payloadBase64=(Hex-B64 $Hex)}
}
function Pair-Sample([string]$Hex=$AdvHex,[int]$Rssi=-60,[string]$Response=$RspHex,[int]$Delay=100,[int]$Candidates=1) {
    $s = Report-Sample $Hex $Rssi
    $s.responseBase64 = Hex-B64 $Response
    $s.responseDelayMs = $Delay
    $s.candidateCount = $Candidates
    $s
}
Preview-Scan $PairProfile @((Pair-Sample))
```

预期预览 `bodyLength=87`、`maximumBodyLength=3070`、`revisionForPreview=1`、首条sample `passed=true`。`bodyBase64` 解码为下列87字节wrapper。预览不会配置设备、制造真实接收时间或证明radio/boot隔离；sample没有RSP RSSI字段，因此RSP=-90的保留必须在实物链路观察。

```text
020113001000010000000000000003a00050008000000001f401013a0001011f0001000442636e2dfffffffffda50693a4e24fb1afcfc6eb07647825ffffffffffffffffffffffffffffffff00b5dd001200ff03000f00
```

实际下发使用 `Send-Scan $p`，revision由平台生成。预览的revision=1不是当前设备写入值。每个BFLT用例分别选择 `nrf54` 和 `esp32` 执行，另一radio关闭或明确隔离其上报；随后才测双路。信号边界以**控制器实际报告的RSSI**判定，发射功率/衰减器设置不等于接收RSSI。精准i8输入需可审计接收边界夹具；-128不能由普通手机广播参数设置。

### 2.3 UART 读回与证据

只在已满足第1节UART独占隔离前置时运行。环境变量 `GW_TEST_COM` 由操作者设为实际端口；安装pyserial属于测试机准备，本轮不安装。

```powershell
python firmware/nrf54l15/tools/uart_probe.py --port $env:GW_TEST_COM status
python firmware/nrf54l15/tools/uart_probe.py --port $env:GW_TEST_COM command 0x1a
python firmware/nrf54l15/tools/uart_probe.py --port $env:GW_TEST_COM command 0x1b
python firmware/nrf54l15/tools/uart_probe.py --port $env:GW_TEST_COM adv status
```

`0x1A`完整读回包含engine、嵌套基础过滤/旧规则、scope/window、V2规则顺序/掩码和revision。revision从wrapper偏移6的小端u64读取。`0x1B`返回96B：`revision:u64, engine:u8, scope:u8, pending_count:u16, pending_bytes:u32, 10×u64计数`。10项依序为seen、passed、rule_dropped、pair_timeout、pair_ambiguous、pair_overflow、malformed_ad、unknown_rssi、unsupported_context、config_discarded；同boot累计，比较前后差值。pending_bytes只计AD载荷，不能当4096B总内存预算。

0x1A/0x1B自身也要求radio_ready；射频默认关闭或cap13不可用时应返回UNSUPPORTED，该环境不满足V2实物验收前置。不能把此响应判为配置丢失，也不能为获得读回而擅自解除板级门控。

整机测试不得为了读回临时抢占UART；保存ESP实际0x19/1A/1B控制事务的被动串口抓取或已审阅的只读调试记录。ESP本机对应状态从 `DIAGNOSTICS` BLE和本机日志取证；没有精确字段的公开端点时不要编造REST路径，可用受控调试记录，注明采集方法。

```powershell
Send-Task 'DIAGNOSTICS' @{profile='BLE';durationSeconds=10}
```

### 2.4 共用恢复 R0

停止可编程对端/故障夹具；恢复原接线和流控；按最初保存的完整目标，逐radio、逐配置组提交新平台事务并确认实际状态。恢复原自动连接规则启停和测试对端状态；关闭临时连接。不得用旧revision强写、擦任务journal或将未交付spool清空“恢复”。独立UART试验产生的临时revision只用于隔离台架：结束后按审核过的供电流程重启nRF，恢复ESP独占管理，由平台/ESP持久目标对账；不启用默认关闭的GPIO8恢复。保存恢复的任务ID、读回、空口/连接状态，未恢复则记录阻塞。

## 3. UART 人工用例

### MAN-UART-01 — 电气参数、黄金帧与分帧

原验收：ACC-01、ESP-AT-01、ESP-AT-34（仅链路前置）。前置：第1节、隔离台架、已验证UART适配器与被动采集。初始状态：**未执行**。

1. 在独立逻辑采集中确认1 Mbaud、8N1、RTS/CTS方向和空闲电平；执行2.3的status，保存HELLO bootId/能力/版本及71B状态。
2. 可编程UART发送器发送固定HELLO黄金帧 `00 05 47 57 01 01 01 09 08 07 06 05 04 03 02 01 01 01 01 05 4F 42 A1 E8 00`，预期回显link=`0x0102030405060708`、msg=0，boot不变。分别按单字节/半帧分割及两帧连续发送；空分隔符不造成错误命令。
3. 用仓库codec在夹具中生成 `encode(0x10, link, msg, bytes([0xff])+bytes(3071))`：3072B payload为最大合法帧，未知opcode应UNSUPPORTED，无越界/射频动作。翻转编码前CRC、构造非法COBS/3073B声明长度/连续3108非零字节，均不执行；下一合法GET_STATUS恢复。不要以CLI自身拒绝超长替代设备拒绝测试。
4. 比较坏帧计数、UART原始字节、命令执行/启动次数，设备不得重启或错误应用配置。源码codec `encode/decode` 在 `uart_probe.py`，没有内置“故障注入”CLI；该步需夹具保留发送程序和输出文件。

证据：双向UART与流控波形、每个分段/坏帧二进制、HELLO/GET_STATUS前后值、无射频副作用抓包。恢复：R0。

### MAN-UART-02 — 控制响应丢失、重复、冲突与 BUSY

原验收：ACC-02、ESP-AT-03/15。前置：MAN-UART-01通过、可按方向丢帧的隔离夹具；合法配置已保存。初始状态：**未执行**。

1. 在同一link内发送GET_STATUS（CMD type0x10、opcode03、msg=1），丢弃首个响应，按原msg/payload重发；应返回同一缓存响应，不重新采样成另一个结果。
2. 保持msg=1，改payload为opcode02；应返回CONFLICT(4)，不执行新命令。再发msg=2正确请求成功；发更旧msg=1不成为新操作。
3. 在已验证的角色冲突条件下（如活动Central连接时要求启动广播），取得明确BUSY(3)；先释放冲突资源，按新msg重试。缺失响应则仍用旧msg，不能混用这两种重试规则。
4. 对广播配置类重放再观察 `startCount/liveUpdates` 与空口，确认相同请求没有第二次启停/更新副作用；同ID不同内容不改变配置。

证据：按link/msg排列的请求/响应、被丢响应、驱动启动/更新计数、实际广播轨迹。恢复：R0；不反复新建云任务模拟UART重试。

### MAN-UART-03 — CTS长阻塞、DMA中止和重握手

原验收：ACC-03、ESP-AT-02/08。前置：已审阅的流控夹具，可在不短接输出的条件下控制接收端CTS；允许的采集负载。初始状态：**未执行**。

1. 先持续采集并每2 s穿插GET_STATUS，保存正常波形。分别阻塞两个发送方向超过驱动超时预算，记录TX中止/取消回调；不能只拔RX线冒充CTS测试。
2. 恢复CTS，发送合法控制请求并等待主机有界重试/新link握手；检查没有帧交叉、DMA缓冲复用、无界内存增长或错误命令。
3. 注入接收突发使RX溢出，随后发分隔符与合法HELLO；设备应恢复帧边界，bad_rx_frames可诊断。控制和ACK可穿插数据，不永久饥饿。
4. 核对同nRF boot新link保留扫描/广播目标、FIFO和待确认批次；旧link非HELLO帧忽略。不得由普通UART超时直接触发默认关闭的硬件复位。

证据：CTS/TX/RX原始波形、TX完成/中止/取消时序、前后boot/link、配置/FIFO、错误计数。恢复：R0，释放全部流控故障。

### MAN-UART-04 — CUSTODY背压和跨link批次重放

原验收：ACC-03/06、ESP-AT-04/05/08/12（仅UART接管子项，完整掉电验收另记）。前置：真实ESP持久队列、可延迟Flash提交和丢ACK的夹具，独立probe不能代替。初始状态：**未执行**。

1. 发送有序可识别广播；阻止一次ESP Flash提交完成。nRF批次保持待确认，ESP不得发送成功CUSTODY；空间不足时按协议BUSY/背压。
2. 允许提交但丢一次CUSTODY；保存本批次原字节、boot/batch_seq及ESP receipt。nRF重发后ESP只重认接管，不新建第二条观测。
3. 在批次待确认时重建UART link，不重启nRF；批次序号/内容保持，旧link ACK无效，新link按持久证据确认。
4. 阻塞上行直到spool压力出现，再恢复；nRF仅在ESP持久接管后释放，ESP仅在合法RAW_PERSISTED后回收上行数据。记录dropped_full与尚未接管损失，不能承诺RAM无限保存。

证据：UART批次/ACK、Flash提交点、稳定eventId/receipt、FIFO/spool曲线、平台逐项ACK。恢复：先恢复提交/联网并确认队列排空，再R0；不清队列。

### MAN-UART-05 — 重启边界与实际能力

原验收：ACC-01/06/17、ESP-AT-08/11/24/35（相应边界子项）。前置：经批准的分路供电/复位夹具，先确认不会反向供电；硬件GPIO8测试仍受独立门控。初始状态：**未执行**。

1. 仅重握手，记录nRF boot保持而link变化；V2完整目标、连接和FIFO按协议保留。读取HELLO能力，射频禁用时不能因文档写了功能而期待非零射频位。
2. 按审核流程仅重启nRF；应产生新boot，扫描/广播从关闭起步，旧连接与旧PAIR上下文无效；ESP对账恢复已提交完整目标。
3. 仅重启ESP而nRF保持：先恢复持久接管和操作journal，再处理旧连接、未交付批次；旧引用不可控制新连接。
4. 整板掉电前分别标记“未CUSTODY/已CUSTODY”样本，恢复后只对已接管记录要求不丢失。未接管的nRF RAM缺口须明确记录，不能伪称已重放。

证据：各芯片电源/启动日志、boot/link矩阵、读回/实际状态、样本责任边界。恢复：R0；所有未知操作先对账，不能自动补做副作用。

## 4. Beacon 掩码专项：与 BFLT-AT-01～16 一一对应

以下16项共用第1节环境、2.2节输入与2.4节恢复要求。每项证据至少包含完整规则和输入字节、预览结果、实际下发revision、设备读回、控制器接收/过滤记录、平台原文及对应计数差值；保存通过与拒绝样本。缺少实际接收或精确边界夹具时，分别记录预览结果与实物子项阻塞，不合并判通过。

本节每例公共前置：第1节及2.2；平台先预览，再在放行台架逐radio应用同一完整profile并由可编程对端重放对应AD。每次保存完整配置、预览、设备读回、0x1B/本机计数前后值、UART原始记录与平台RawEvent，并检查未选位仍按原值回显。每例恢复均执行R0；改变报文后还原公共ADV/RSP。**每例初始状态均为未执行**，下列逐例再次列示。没有夹具支持的子项不得以预览替代实物通过。

### MAN-BFLT-01 — 五字段AND及原始双报告保留

映射：BFLT-AT-01、BFLT-001/007/010/017/018；ESP-AT-19；ACC-04。状态：**未执行**。

1. `Preview-Scan $PairProfile @((Pair-Sample))`，检查87B和ALLOW；`Send-Scan $PairProfile`，核对完整读回。
2. 唯一ADV=-60、RSP=-90，RSP在100ms后到达；预期依序放行原ADV、原RSP，两条不同序号，时间/RSSI不互相覆盖，payload分别等于公共hex。
3. 将scope切为REPORT并删除pairWindowMs，仍一条五字段AND；两报告各自缺字段，均不通过，不能借历史名称。复位为PAIR后再分别在两radio重做。

证据：两条源记录、各capture时间/RSSI与上传payload；passed增2。恢复：R0。

### MAN-BFLT-02 — Major/Minor部分位掩码

映射：BFLT-AT-02、BFLT-004/009；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = Copy-Json $PairProfile
$a = $AdvHex.Replace('12340003c5','12ff00a3c5')
$b = $AdvHex.Replace('12340003c5','13000003c5')
$c = $AdvHex.Replace('12340003c5','12340004c5')
Preview-Scan $p @((Pair-Sample $a),(Pair-Sample $b),(Pair-Sample $c))
```

1. 核对Major value4608/mask65280、Minor value3/mask15。三个输入顺序重放，每对间隔>500ms且无残留候选。
2. 预期通过/拒绝/拒绝；空口Major/Minor大端，UART数值小端，不能互换。加入Major value4863（0x12FF）但mask不变，应相同匹配且完整回读保留4863。
3. 对照UART记录/空口与平台结果；拒绝原因计规则拒绝，不伪装协议损坏。恢复：R0。

### MAN-BFLT-03 — UUID掩码与字节序

映射：BFLT-AT-03、BFLT-003/014；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = Copy-Json $PairProfile
$p.beaconFilterV2.rules[0].ibeaconUuid.maskHex = 'ffffffff000000000000000000000000'
$a = $AdvHex.Replace('fda50693a4e24fb1afcfc6eb07647825','fda50693000000000000000000000000')
$b = $a.Replace('fda50693','fca50693')
Preview-Scan $p @((Pair-Sample $a),(Pair-Sample $b))
```

1. 应为通过/拒绝；再发送整串倒序UUID，不得匹配选中前4字节。
2. 真机应用同profile并重放，核对空口UUID从左到右的规范字节与wrapper掩码，未掩位不被“归一化”清零。
3. 保存完整读回和三组输入/输出，恢复R0。

### MAN-BFLT-04 — 名称PREFIX、EXACT、大小写与字节掩码

映射：BFLT-AT-04、BFLT-002；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = Copy-Json $PairProfile
Preview-Scan $p @((Pair-Sample))
$p.beaconFilterV2.rules[0].name.mode = 'EXACT'
Preview-Scan $p @((Pair-Sample))
$p.beaconFilterV2.rules[0].name.mode = 'PREFIX'
Preview-Scan $p @((Pair-Sample -Response '070962636e2d3031'))
$p.beaconFilterV2.rules[0].name.maskBase64 = Hex-B64 'dfffffff'
Preview-Scan $p @((Pair-Sample -Response '070962636e2d3031'))
```

1. 依次预期通过/拒绝/拒绝/通过；最后只屏蔽首字节大小写差的0x20位，不是实现自动忽略大小写。
2. 真机按四个完整profile逐次验证。UTF-8名称使用字节长度；可增加多字节名称前缀并保存原始Base64，不把字符数当长度。
3. 证据为AD type09、原始名称字节、mask回显及结果。恢复R0。

### MAN-BFLT-05 — 名称来源和歧义

映射：BFLT-AT-05、BFLT-002/008；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = Copy-Json $PairProfile
$short = '070842636e2d3031'
foreach ($source in @('COMPLETE','SHORTENED','ANY')) {
    $p.beaconFilterV2.rules[0].name.source = $source
    Preview-Scan $p @((Pair-Sample -Response $short))
}
$p.beaconFilterV2.rules[0].name.source = 'ANY'
Preview-Scan $p @((Pair-Sample -Response '070942636e2d3031070958626e2d3031'))
```

1. 仅type08时预期拒绝/通过/通过；两个冲突type09名称必须拒绝，不能任取一个。
2. 再发送相同名称重复type09应可视为同一候选；ANY同时有08与09时优先完整名称，不拿匹配的08覆盖不匹配的09。
3. 真机记录原始RSP及冲突原因，不把缩短名称解释为完整名称。恢复R0。

### MAN-BFLT-06 — RSSI RANGE端点与未知值

映射：BFLT-AT-06、BFLT-005；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = New-ReportProfile 4
Preview-Scan $p @((Report-Sample -Rssi -76),(Report-Sample -Rssi -75),(Report-Sample -Rssi -35),(Report-Sample -Rssi -34),(Report-Sample -Rssi -128))
```

1. 顺序预期拒绝/通过/通过/拒绝/UNKNOWN_RSSI拒绝。
2. 在屏蔽/衰减环境记录实际控制器RSSI；只对确实捕获到的端点样本判定。-128和精确单样本需接收输入夹具，缺少则子项阻塞。
3. 再用PAIR profile保持ADV=-60、RSP=-90，只以ADV判定；改变iBeacon校准字节C5不能改变接收RSSI条件。证据为控制器元数据与计数。恢复R0。

### MAN-BFLT-07 — RSSI补码BITS

映射：BFLT-AT-07、BFLT-005；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = New-ReportProfile 4
$p.beaconFilterV2.rules[0].rssi = @{mode='BITS';valueDbm=-64;mask=240}
Preview-Scan $p @((Report-Sample -Rssi -65),(Report-Sample -Rssi -64),(Report-Sample -Rssi -49),(Report-Sample -Rssi -48),(Report-Sample -Rssi -128))
```

1. 顺序预期拒绝/通过/通过/拒绝/UNKNOWN_RSSI拒绝，明确不是“RSSI≥-64”。
2. 两radio按相同接收输入执行；另设只有DENY规则且RSSI启用，未知RSSI仍不能通过默认放行动作绕过。
3. 保存value补码C0、maskF0、每条原始RSSI和读回；实物端点要求同MAN-BFLT-06。恢复R0。

### MAN-BFLT-08 — 规则首命中与同条AND

映射：BFLT-AT-08、BFLT-001/006/016；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$name = New-ReportProfile 1
$uuid = New-ReportProfile 2
$or = Copy-Json $name
$or.beaconFilterV2.rules = @($name.beaconFilterV2.rules[0],$uuid.beaconFilterV2.rules[0])
$and = New-ReportProfile 3
Preview-Scan $or @((Report-Sample $AdvHex),(Report-Sample $RspHex))
Preview-Scan $and @((Report-Sample $AdvHex),(Report-Sample $RspHex))
```

1. 两条ALLOW：仅UUID或仅名称输入均通过；单条二字段AND：两输入均拒绝。本例把名称AD放入LEGACY_ADV，以独立REPORT验证规则逻辑。
2. 将相同UUID条件的DENY放在ALLOW之前，应拒绝；互换顺序则先ALLOW。只有DENY且不命中时默认放行；有ALLOW无命中则拒绝。
3. 平台编辑/预览、完整wrapper和两端结果必须保持规则顺序，不把多字段界面拆成多条ALLOW。证据记录matchedRuleIndex与设备输出。恢复R0。

### MAN-BFLT-09 — PAIR 500/501ms及上下文隔离

映射：BFLT-AT-09、BFLT-007/008/018；ESP-AT-11/19；ACC-04。状态：**未执行**。

```powershell
Preview-Scan $PairProfile @((Pair-Sample -Delay 500),(Pair-Sample -Delay 501))
```

1. 预览分别通过/PAIR_TIMEOUT。用可编程接收夹具产生capture_ms=1000/1500与1000/1501，且有唯一前序ADV；前者即使worker延迟也必须配对，后者不能复活过期ADV。
2. 无新流量时等待生产者水位严格超过1500，原ADV超时释放；不能在恰1500提前过期已发布响应。保存回调入队、worker消费与水位轨迹，不能仅看服务器到达时间。
3. 分别只改变radio、boot、实际地址、地址类型、scanRevision，均不关联；ADV和RSP分别来自两radio不构成PAIR。随机地址变更不得借bond identity拼接。
4. 自然空口的SCAN_RSP时序不能可靠制造500ms控制器边界，预览也不含boot/radio隔离输入；无相应夹具记该部分BLOCKED。恢复R0并清理易失待配上下文。

### MAN-BFLT-10 — 歧义、缺响应与DENY默认动作

映射：BFLT-AT-10、BFLT-006/007/008；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
Preview-Scan $PairProfile @((Pair-Sample -Candidates 0),(Pair-Sample -Candidates 2))
$p = Copy-Json $PairProfile
$p.beaconFilterV2.rules[0].action = 'DENY'
Preview-Scan $p @((Report-Sample)) # 没有response的PAIR样本
```

1. 用同实际地址在窗口内发两条未配ADV再一个RSP；三条均不放行，不能取最近ADV。零候选RSP同样拒绝，记录歧义计数的原始报告条数。
2. 只有DENY规则时发ADV不发RSP，超过500ms后仍丢弃；不是完整上下文的默认ALLOW。
3. 对单个唯一完整对使用不命中的DENY条件，才允许正常默认放行两条。证据：候选数量、三条capture时间、计数/输出、无无限待定。恢复R0。

### MAN-BFLT-11 — 基础过滤与V2取交集

映射：BFLT-AT-11、BFLT-005/016；ESP-AT-19；ACC-04。状态：**未执行**。

```powershell
$p = Copy-Json $PairProfile
$p.filters.minRssi = -50
Preview-Scan $p @((Pair-Sample -Rssi -60),(Pair-Sample -Rssi -50),(Pair-Sample -Rssi -35))
```

1. 应拒绝/通过/通过，有效RSSI区间[-50,-35]。界面/预览与读回保留minRssi，不自动清除。
2. 设置基础manufacturerIds为 `[76]` 应命中公共iBeacon；改为 `[65535]` 则基础过滤拒绝。服务列表/厂商列表内部OR，基础类别之间AND。
3. 两radio分别验证完整配置交集与实际采集，RSP RSSI不改变PAIR的ADV条件。恢复R0。

### MAN-BFLT-12 — 服务16/32/128位归一与iBeacon同一结构约束

映射：BFLT-AT-12、BFLT-003/009；ESP-AT-19/22；ACC-04/07。状态：**未执行**。

```powershell
$p = New-ReportProfile 32
$p.beaconFilterV2.rules[0].serviceUuid = @{value='0000180f-0000-1000-8000-00805f9b34fb';maskHex='ffffffffffffffffffffffffffffffff'}
$s16 = '03030f18'
$s32 = '05050f180000'
$s128 = '1107fb349b5f80000080001000000f180000'
$fakeService = $AdvHex.Replace('fda50693a4e24fb1afcfc6eb07647825','0000180f00001000800000805f9b34fb')
Preview-Scan $p @((Report-Sample $s16),(Report-Sample $s32),(Report-Sample $s128),(Report-Sample $fakeService))
```

1. 前三项通过，iBeacon厂商UUID不能冒充服务，第四项拒绝。另用Service Data AD `03160f18`、`05200f180000`、`1121fb349b5f80000080001000000f180000`重复归一。
2. 同一规则要求UUID+Major+Minor时，构造两个合法FF结构：A只有UUID满足，B只有Major/Minor满足，整体不得拼字段通过。两个完整iBeacon超过31B，当前不支持完整扩展空口接收；此子项必须使用生产共享引擎接收夹具或平台预览，并明确不能计为本机扩展射频通过。
3. 对无效公司头、Data非恰25B、AD越界、非零尾填充分别记录拒绝；不把mask应用于iBeacon格式识别头。保存各位宽原始AD与规范UUID。恢复R0。

### MAN-BFLT-13 — 严格校验与3070/3071字节总限额

映射：BFLT-AT-13、BFLT-001/012；ESP-AT-01/19；ACC-01/04。状态：**未执行**。

1. 分别复制公共profile后只改一项：major.mask=0；name mask少一字节；fieldMask=30却保留name；fieldMask=95含未知bit；RSSI value=-128。逐项预览/提交，应拒绝且目标revision/完整读回不变，不能截短或忽略字段。
2. 使用下列合法满边界配置，预览bodyLength必须3070；再最后两行制造3071，必须拒绝。该profile用于容量，不承诺现场普通Beacon命中它；先预览再按授权台架验证设备解码。

```powershell
$p = New-ReportProfile 63
$r = $p.beaconFilterV2.rules[0]
$r.name.valueBase64 = Hex-B64 ('01' * 29)
$r.name.maskBase64 = Hex-B64 ('ff' * 29)
$r.serviceUuid = @{value='0000180f-0000-1000-8000-00805f9b34fb';maskHex=('ff' * 16)}
$p.beaconFilterV2.rules = @(0..7 | ForEach-Object { Copy-Json $r })
$p.beaconFilterV2.rules[0].name.valueBase64 = Hex-B64 ('01' * 28)
$p.beaconFilterV2.rules[0].name.maskBase64 = Hex-B64 ('ff' * 28)
$p.beaconFilterV2.rules[1].fieldMask = 59
$p.beaconFilterV2.rules[1].Remove('rssi')
$p.filters.serviceUuids = @(0..103 | ForEach-Object { '{0:x8}-0000-1000-8000-00805f9b34fb' -f $_ })
$p.filters.manufacturerIds = @(0..123)
$edge = Preview-Scan $p
$edge.bodyLength # 3070
$p.beaconFilterV2.rules[0].name.valueBase64 = Hex-B64 ('01' * 27)
$p.beaconFilterV2.rules[0].name.maskBase64 = Hex-B64 ('ff' * 27)
$p.beaconFilterV2.rules[1].fieldMask = 63
$p.beaconFilterV2.rules[1].rssi = @{mode='RANGE';minDbm=-75;maxDbm=-35}
Preview-Scan $p # 3071：拒绝
```

3. nRF非法长度可将保存的3070B body追加一个00后通过0x19台架发送，预期BAD_PARAM；不要把3071B直接解释成“超3072 payload”，它仍须由wrapper长度校验拒绝。ESP/平台分别保留拒绝证据，不能只测一层。

证据：各请求JSON与body长度/摘要、错误响应、失败前后完整wrapper、无配置空窗。恢复R0。

### MAN-BFLT-14 — legacy↔V2原子切换、revision和应用回滚

映射：BFLT-AT-14、BFLT-011/014/015；ESP-AT-03/15/19；ACC-02/04。状态：**未执行**。

```powershell
$legacy = @{scan=@{enabled=$true;active=$true;intervalMs=100;windowMs=50};filters=@{minRssi=$null;serviceUuids=@();manufacturerIds=@()};beaconFilterV2=$null;beaconRules=@(@{action='ALLOW';reportKinds=@('LEGACY_ADV');adType=255;offset=0;valueBase64='TAA=';maskBase64='//8='})}
Send-Scan $legacy
# 等实际任务终结并读回后再执行下一行
# Send-Scan $PairProfile
```

1. 开启持续可识别输入。旧→V2在同一radio扫描补丁同时带 `beaconRules:[]` 和完整V2；反向同时带 `beaconFilterV2:null` 和目标旧规则。每次读完整0x1A，不能用0x02子集代替。
2. 隔离UART台架从当前wrapper读取revision，在同一boot重送0x19同revision/同body应幂等；同revision改变一个未掩位仍CONFLICT；旧revision应STALE。该步骤使用当前动态revision，不把87B向量的1强写线上。
3. V2活动时旧0x01/0x05/0x06应CONFLICT；0x02只能基础读取。模拟驱动应用失败，完整旧profile及revision恢复，切换期间旧PAIR缓存清理；若恢复失败，状态应明确FAULT，不能宣称成功。
4. 逐帧对照持续输入与切换边界，未曾生效的无过滤状态不得放行不合格报文。驱动失败点需可审计夹具，没有则阻塞子项。

证据：每次完整body/hash、status、revision、实际状态、config_discarded和空口/上报时间线。恢复R0，不清持久任务记录。

### MAN-BFLT-15 — 有界缓存、计数、停扫与重启

映射：BFLT-AT-15、BFLT-008/010/015；ESP-AT-11/12/19/33；ACC-04/18（负载子项）。状态：**未执行**。

1. PAIR模式在500ms内注入32个不同地址的唯一ADV，均不发RSP；读取pending_count不超过32。第33个新ADV被拒绝并计pair_overflow，不驱逐前32项冒充成功。
2. 分别检查输入队列满与PAIR缓存满；seen/终态原因按原始报告条数，无重复计数。每radio实际总缓存≤4096B的证明须结合目标map/运行内存证据；pending_bytes只计AD，当前32条legacy输入通常先触发条目上限，不能伪称已从空口触及4096B。
3. 超时后pending归零；停扫、完整profile切换、Central维护暂停、nRF重启分别穿插旧ADV/新RSP，应不复用旧名称。same-boot计数累计，重启计数起点改变并绑定新boot。
4. 规则放行后人为使后续FIFO满：records_passed和dropped_full各自解释，passed不等于已CUSTODY或上传成功。

证据：地址/捕获顺序、队列水位、96B计数快照、目标内存记录、实际输出和丢弃原因。恢复：排空/超时并R0。

### MAN-BFLT-16 — 能力混合、完整恢复与不近似降级

映射：BFLT-AT-16、BFLT-011～014/017；ESP-AT-19/34；ACC-04/17。状态：**未执行**。

1. 在测试签名清单/运行能力分别缺nRF V2、缺ESP V2的专用网关上预览/提交相应radio；应明确拒绝，另一radio目标不被覆盖。不能通过手改客户端JSON给自己授予capability。
2. 当前radio_ready为真nRF源码能力包含bit13；从真实HELLO取证，不照抄旧文档的0x0E1E或“bit13发送0”历史文字。默认RF关闭仍保持射频能力0。
3. 旧设备继续用旧规则；V2请求不转换成多条旧ALLOW。旧平台环境只下旧目标，不据新版文档期待旧部署接受新字段。
4. 保存完整V2wrapper，在ESP/nRF重启、Central临时暂停恢复后逐字段对照engine/scope/window/rule顺序/所有mask/basic/revision；旧待配上下文不恢复。降级到无V2固件前必须有受授权旧目标迁移；不能无损迁移则阻止降级。

证据：签名清单与真实HELLO/本机能力交集、拒绝记录、双radio前后目标、完整wrapper、空口恢复。恢复R0；不永久修改生产授权。

## 5. 双射频、服务器广播与 Central/GATT

### MAN-BLE-01 — 两路扫描、主动/被动和原始数据

原验收：ESP-AT-19/33、ACC-04/18。前置：两radio各自RF放行，关闭广播和连接；初始状态：**未执行**。

```powershell
$scan = @{scan=@{enabled=$true;active=$true;intervalMs=100;windowMs=50};filters=@{minRssi=$null;serviceUuids=@();manufacturerIds=@()};beaconRules=@();beaconFilterV2=$null}
$radio='nrf54'; Send-Scan $scan
# 等终态后 $radio='esp32'; Send-Scan $scan
```

1. 先只nRF扫描、再只ESP扫描、最后双路；对端发公共ADV/RSP与服务UUID AD。各源上报原始字节、类型、地址、RSSI、捕获时间正确。
2. 把active改false，按实际空口确认不主动请求SCAN_RSP；不能因以前缓存里有名称而补造RSP。重新active=true后应恢复实际响应。
3. 改一radio过滤/启停，另一radio完整目标保持。持续Wi-Fi业务/Flash写入时记录扫描空档、丢弃、资源曲线，不设无来源吞吐通过门槛；项目冻结负载指标未提供则长稳子项阻塞。

证据：空口ADV/SCAN_REQ/SCAN_RSP、每源UART/本机记录、上行RawEvent、实际扫描状态。恢复R0。

### MAN-BLE-02 — 双射频50/150ms一对一去重

原验收：ESP-AT-09/10/11/33、ACC-05。前置：MAN-BLE-01、有效nRF↔ESP单调时间锚、可编程双源输入夹具；初始状态：**未执行**。

1. 同地址/类型、同报告kind、同原始AD、可比较时间各一条，满足 `abs(mappedA-mappedB)+errorA+errorB <= 50ms`；应合成一个事件，`capture.radio=NRF54`、`observedBy=["NRF54","ESP32"]`，保留nRF载荷/元数据。
2. 输入误差各2ms，时间差46ms应可配、47ms不可配；不能只检查裸时间差≤50。设置多个同差候选，验证按最小差、入队次序一对一，无一条匹配多条。
3. 让单源候选到达等待150ms后定稿，再注入迟到另一源；已定稿eventId/来源/时间不改，迟到项独立。边界按实际代码的到达时刻与定稿执行记录核验，不以平台网络延迟代替。
4. 同源重复、不同AD/报告类型/地址类型、未知地址、不可比较锚或boot改变均不错误合并。PAIR过滤先执行，不能把两radio各缺一半的字段在去重层拼成合格上下文。

证据：两源原始capture、锚误差、入队/定稿时间、唯一eventId映射表；自然空口统计另存，不能代替精确边界夹具。恢复R0并重建有效时间锚。

### MAN-BLE-03 — 服务器开关Beacon/自定义广播和频次

原验收：ESP-AT-20、ACC-09。前置：各radio具ADV能力，停止该radio扫描和Central/Peripheral连接，具空口抓包；初始状态：**未执行**。

```powershell
$adv = @{enabled=$true;mode='NONCONNECTABLE';intervalMs=250;advDataBase64=(Hex-B64 $AdvHex);scanResponseBase64=''}
Send-Configuration @{radios=@{$radio=@{advertising=$adv}}}
# 任务终结后分别执行以下新事务，不合并到扫描组。
# Send-Configuration @{radios=@{$radio=@{advertising=@{enabled=$false}}}}
# Send-Configuration @{radios=@{$radio=@{advertising=@{enabled=$true;intervalMs=1000}}}}
```

1. 两radio分别使用上述iBeacon，在ACTIVE后抓取UUID/Major/Minor/校准值；关闭后STOPPED且空口停止，重新开启保留原内容。
2. 依次用intervalMs=100、250、1000、10240，核对设备有效配置与足量空口间隔分布。单位是ms，不能把250解释成250Hz；无线随机延时/调度存在，按控制器/测试规范的允许区间判断，不要求每包严格250.000ms。
3. 自定义Service Data：将advDataBase64改为 `Hex-B64 '02010605160f180102'`，mode改SCANNABLE_NONCONNECTABLE，scanResponseBase64=`Hex-B64 $RspHex`。主动标准扫描器应看到原始内容及名称RSP。
4. 试intervalMs=99、超过31B、不完整AD、NONCONNECTABLE带非空RSP，预期明确拒绝且旧目标/空口保持。三种mode的第三种在MAN-BLE-04测试。

证据：平台任务结果、0x08/0x0C或本机完整读回、ACTIVE/STOPPED、原始空口载荷与频次统计。恢复R0。

### MAN-BLE-04 — 广播热更新、可连接模式、PAUSED与FAULT

原验收：ESP-AT-20/21/25（角色子项）、ACC-09/10/11（角色子项）。前置：ADV+PERIPHERAL能力与板授权、标准BLE测试客户端（无需业务Security2）、驱动失败夹具；初始状态：**未执行**。

1. 延续MAN-BLE-03运行中广播，将内容里的Major=0x1234改0x1235，随后改interval；核对原请求回显、实际配置/量化值及空口新内容，没有只更新一半却报成功。
2. 关闭广播后配置 `mode=CONNECTABLE_SCANNABLE`、AD=`020106`、RSP=`070942636e2d3031`，再开启。标准客户端建立真实BLE连接；公共INFO可用于确认服务存在，本例不把连接视为业务授权。
3. 连接造成广播PAUSED时，实际连接必须活跃，目标enabled=true保留；服务器关闭广播应STOPPED且不误断已有连接；再开启时允许有证据的PAUSED，断开后恢复ACTIVE。保存0x0D/本机真实连接、0x0C/配置前后两次一致读回。
4. 按已审阅夹具在载荷更新/恢复旧载荷处分别注入失败：前者恢复完整旧目标，恢复也失败则明确FAULT并停止，不假报VERIFIED。没有驱动失败注入工具则该子项BLOCKED。

证据：标准客户端连接记录、空口连接/广告、真实连接和状态转换、最后驱动错误/更新次数、平台任务证据。恢复：关闭测试连接、去掉失败夹具，R0；不擦bond或业务凭据。

### MAN-BLE-05 — 手动Central连接、服务/特征和16/32/128UUID

原验收：ESP-AT-22/23、ACC-07/08。前置：可编程GATT对端，已登记实际稳定地址，Central/服务发现能力、权限、可信时钟；广播/Peripheral先停止。初始状态：**未执行**。

```powershell
$peerAddress = $env:GW_TEST_PEER_ADDRESS # 真实12位大写hex，不带冒号
$peerType = 'PUBLIC' # 若为真实random static则改RANDOM
$radioName = 'NRF54' # 支持本机Central的测试轮改ESP32
Send-Task 'GATT_CONNECT' @{radio=$radioName;deviceAddress=$peerAddress;addressType=$peerType;security='UNENCRYPTED';timeoutMs=10000}
# 任务成功后从实际结果复制connectionRef，不以示例字符串代替。
# $connectionRef = '实际32位小写hex'
$read = @{radio=$radioName;deviceAddress=$peerAddress;addressType=$peerType;serviceUuid='0000180f-0000-1000-8000-00805f9b34fb';characteristicUuid='00002a19-0000-1000-8000-00805f9b34fb';timeoutMs=10000}
# $read.connectionRef=$connectionRef
# Send-Task 'GATT_READ' $read
```

1. 对端实现Battery Service/Level等已知只读值，逐项连接/读取；成功必须有真实地址、MTU、连接状态、安全状态证据，UART受理不算已连接。
2. 广播服务字段分别使用 `03030f18`、`050578563412`、`110778563412f0debc9a7856341278563412`，平台应解析wireBits16/32/128及各规范UUID。32位12345678仅测试解码；自定义实际GATT服务采用128位UUID，不冒称SIG分配32位服务。实际发现必须与对端真实服务表一致。
3. 对不存在服务/特征、对端地址不符、错误安全状态分别验证失败与关闭，无虚构成功。未有bond时BONDED_ENCRYPTED不得静默退为明文；已有bond场景须提供此前受控绑定证据，不触发自动首次配对。
4. nRF建立Central期间扫描PAUSED，断开并释放后恢复完整扫描目标；保持连接不宣称并行扫描。另一radio目标独立。

证据：真实对端服务表/值、空口ATT、UART动作结果/连接信息、平台结果、扫描暂停恢复的完整profile。恢复R0。

### MAN-BLE-06 — 自动连接授权、新鲜度及仅RSP带UUID

原验收：ESP-AT-22、ACC-07。前置：MAN-BLE-05，nRF扫描、采集上下文与服务发现能力均放行；目标固定NRF54；稳定public/random static对端。初始状态：**未执行**。

```powershell
$createHeaders=$headers.Clone(); $createHeaders['Idempotency-Key']=[guid]::NewGuid().ToString()
$peerBody=@{scopeId=$env:GW_TEST_SCOPE_ID;displayName='manual-battery-peer';deviceAddress=$peerAddress;addressType=$peerType} | ConvertTo-Json
$peer=Invoke-RestMethod -Method Post -Uri "$api/api/v1/ble-peers" -Headers $createHeaders -ContentType 'application/json' -Body $peerBody
$peerId = $peer.peerId
if (-not $peerId) { throw '未取得实际peerId，停止创建规则并检查响应' }
$ruleBody=@{name='manual-battery-auto';peerId=$peerId;gatewayIds=@($gateway);enabled=$true;priority=100;serviceUuid='0000180f-0000-1000-8000-00805f9b34fb';security='UNENCRYPTED';wireBits=16;minRssi=-85;maxAgeMs=5000;cooldownMs=30000} | ConvertTo-Json -Depth 10
$ruleHeaders=$headers.Clone(); $ruleHeaders['Idempotency-Key']=[guid]::NewGuid().ToString()
# 确认peerId与权限后执行：
# Invoke-RestMethod -Method Post -Uri "$api/api/v1/auto-connect-rules" -Headers $ruleHeaders -ContentType 'application/json' -Body $ruleBody
Invoke-RestMethod -Uri "$api/api/v1/gatt-connections" -Headers $headers
```

1. 先保持规则关闭/不存在，发相同服务ADV，不自动连接。启用经授权规则后发新鲜可连接nRF ADV，应产生单一GATT_CONNECT并实际发现目标服务。
2. 只在RSP发 `03030f18`，ADV仍真实可连接；使用主动扫描且前置过滤保留两类。500ms内同实际地址/类型的合法证据可关联；丢前序ADV、仅ESP ADV、不可连接ADV、未知/RPA地址、过期或不可信时间不得自动连接。
3. 重复上传同eventId/同rule revision不产生重复任务；新鲜事件受30 s冷却和同对端占用约束。不要通过公开任务接口手填autoTrigger；它只能由平台生成。
4. 在任务到达前使本机观测过期/芯片boot改变，ESP再次校验证据并拒绝STALE_OBSERVATION；发现服务不存在时关闭连接。成功核对ruleId、advEventId、实际对端、connectionRef、MTU、安全状态和发现UUID。

证据：原始ADV/RSP的eventId/capture、解析wireBits/UUID、规则revision/任务唯一键、真实连接和发现、失败理由。恢复：以当前ETag经现有规则PATCH关闭测试规则（保存原启停状态，不删除生产规则），关闭测试连接并R0。

### MAN-BLE-07 — GATT读写、Notify/Indicate及UNKNOWN防重

原验收：ESP-AT-03/15/23、ACC-08。前置：受控对端提供可读写/通知/指示测试特征与**物理写入计数器**，无真实执行器副作用；MAN-BLE-05通过。初始状态：**未执行**。

```powershell
$serviceUuid=$env:GW_TEST_SERVICE_UUID
$characteristicUuid=$env:GW_TEST_CHARACTERISTIC_UUID
$write=@{radio=$radioName;deviceAddress=$peerAddress;addressType=$peerType;connectionRef=$connectionRef;serviceUuid=$serviceUuid;characteristicUuid=$characteristicUuid;timeoutMs=10000;valueBase64='AQID';writeMode='WITH_RESPONSE';verification='READBACK'}
Send-Task 'GATT_WRITE' $write
# 分别在前一任务终结后执行订阅/退订；对端必须实际支持相应mode。
# Send-Task 'GATT_SUBSCRIBE' @{radio=$radioName;connectionRef=$connectionRef;serviceUuid=$serviceUuid;characteristicUuid=$characteristicUuid;mode='NOTIFY';timeoutMs=10000}
# Send-Task 'GATT_UNSUBSCRIBE' @{radio=$radioName;connectionRef=$connectionRef;serviceUuid=$serviceUuid;characteristicUuid=$characteristicUuid;timeoutMs=10000}
```

1. 写010203并同连接读回，验证WRITE响应/READBACK证据和真实计数；超MTU或不支持权限须明确失败。未支持APPLICATION_REPLY适配器不可编造业务验证。
2. 分别NOTIFY/INDICATE订阅，核对CCC实际生效、特征句柄与原特征值RawEvent；退订后不产生新的该订阅通知，已捕获队列仍按原身份交付。
3. 在物理写出后丢响应/断链/ESP复位；能证明完成则终态，否则UNKNOWN。相同taskId/digest和原幂等请求重试不再次写；以对端计数证明，不能只看UI显示一次。
4. 全连接/发现/写/读共用timeoutMs，不给每步重新发10 s预算。UNKNOWN未对账时不重复创建副作用任务；平台只允许诊断继续。

证据：对端物理写入计数、ATT抓包、journal和稳定结果ID、通知connectionRef/handle、超时全过程。恢复：诊断/对账UNKNOWN，退订并关闭连接，R0。

### MAN-BLE-08 — 实际断链、旧引用与暂停目标恢复

原验收：ESP-AT-11/21/24、ACC-08/09。前置：已有活连接/订阅和待交付通知，受控分路供电，完整V2目标已保存。初始状态：**未执行**。

```powershell
Send-Task 'GATT_DISCONNECT' @{radio=$radioName;connectionRef=$connectionRef;timeoutMs=10000}
```

1. 正常断开，以控制器实际CLOSED/旧连接释放完成，不以DISCONNECT命令受理完成。平台收到GATT_LINK_DOWN，旧引用/订阅失效。
2. 对端主动掉电/断开、nRF重启、ESP独立重启分别验证DISCONNECTED/NRF_RESTART/ESP_RESTART来源；当前查询返回的connectionRef与事件一致。
3. ESP重启但nRF旧连接仍在时先journal对账，再关闭旧连接；队列里的旧通知保留捕获时旧boot/connectionRef，不贴新连接身份。重新连接产生新引用；拿旧引用读写/退订应STALE_CONNECTION且无实际动作。
4. 对当前PAIR扫描目标验证暂停和恢复：完整wrapper/revision不变，旧待配名称清理，新的唯一ADV/RSP才可通过。活动广播/Peripheral资源冲突必须明确协调，不默认启用未声明并行位。

证据：空口断开、控制器状态/conn_id、boot变化、旧通知和新连接引用映射、GATT_LINK_DOWN、完整扫描恢复及新样本。恢复：关闭测试订阅/连接并R0，保存最终无残留占用证据。

## 6. 覆盖索引和判定边界

| 本篇编号 | 原验收映射 |
| --- | --- |
| MAN-UART-01 | ACC-01；ESP-AT-01/34部分 |
| MAN-UART-02 | ACC-02；ESP-AT-03/15部分 |
| MAN-UART-03 | ACC-03；ESP-AT-02/08 |
| MAN-UART-04 | ACC-03/06；ESP-AT-04/05/08/12部分 |
| MAN-UART-05 | ACC-01/06/17；ESP-AT-08/11/24/35部分 |
| MAN-BFLT-01～16 | 与BFLT-AT-01～16逐号一一对应；ESP-AT-19专项全覆盖，其他交叉映射见各例 |
| MAN-BLE-01 | ACC-04/18；ESP-AT-19/33部分 |
| MAN-BLE-02 | ACC-05；ESP-AT-09/10/11/33部分 |
| MAN-BLE-03 | ACC-09；ESP-AT-20 |
| MAN-BLE-04 | ACC-09/10/11部分；ESP-AT-20/21/25部分 |
| MAN-BLE-05 | ACC-07/08；ESP-AT-22/23部分 |
| MAN-BLE-06 | ACC-07；ESP-AT-22 |
| MAN-BLE-07 | ACC-08；ESP-AT-03/15/23部分 |
| MAN-BLE-08 | ACC-08/09；ESP-AT-11/21/24 |

这里的“覆盖”是交接步骤映射，**不是已执行或已通过**。ESP-AT-04～08/12的完整Flash故障矩阵、ESP-AT-25业务安全、ESP-AT-33冻结负载长稳、ACC-17目标构建和ACC-18RF性能仍需对应专项记录；本篇不能替代。CASE只要含未执行的必要子项就不能汇总为全通过。

依据： [过滤规范](../beacon-filter-spec.md)、[UART协议](../../server/docs/gateway-uart-protocol.md)、[服务器协议](../../server/docs/gateway-protocol.md)、[ESP验收计划](../esp32s3-validation-plan.md)、[系统验收](../gateway-requirements.md)、[nRF验证边界](../../firmware/nrf54l15/VALIDATION.md)。接口核对源码包括 `gateway_integration.go` 的scan-profile-preview、`beacon_v2.go/beacon_preview.go`、OpenAPI JSON、`gw_filter_v2.c/gw_radio.c`、`gw_scan_config.c`、`gw_capture_rules.c` 和 `uart_probe.py`。本篇以实际0.3.0代码能力门控和完整读回为准，不沿用旧章节中“尚未接入/bit13恒0”的历史描述。
