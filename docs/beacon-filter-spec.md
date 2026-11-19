# Beacon 组合掩码过滤规范

| 项目 | 约定 |
| --- | --- |
| 规范 | BFLT-2.0；文档 1.0，2026-11-19；共同基线 GW32-E2E-1.1 |
| 交付状态 | **三端源码、Schema和预览已实现**；ESP/nRF 0.3.0；软件验证与实板放行见[交付记录](gateway-alignment-implementation.md) |
| 对象 | 平台配置与预览、ESP32-S3 本机过滤及 UART 适配、nRF54L15 过滤引擎 |
| 保留项 | 原 `filters`、`beaconRules`、原始 AD 上报、独立 radio、双路去重、广播/GATT/配网/OTA 功能及原需求编号 |
| 权威映射 | [统一标准](gateway-end-to-end-standard.md)、[开发闭合方案](gateway-alignment-development-plan.md)、[UART扩展](../server/docs/gateway-uart-protocol.md)、[平台协议](../server/docs/gateway-protocol.md) |

## 1. 当前能力和为什么需要 V2

现有 `beaconRules` 每条只匹配一个 AD 结构内的一段 `valueBase64/maskBase64`，多条规则按顺序首条命中，不是多字段 AND。现有 `filters.minRssi` 是数值阈值；RSSI 是接收元数据，不在 AD Data 中。

当前一条原始掩码规则已能同时检查 iBeacon UUID、Major、Minor；名称位于另一个 AD 结构或 SCAN_RSP 时，不能靠追加第二条 ALLOW 实现“名称且 UUID 且 Major”。名称精确长度、名称与其他字段组合、RSSI 位掩码及显式跨报告配对由本版新增能力承担。禁止把这些新语义塞入旧字段并声称兼容。

用户指定的过滤字段均纳入掩码模型：**fieldMask 决定哪些字段参与；字段内容 mask 决定比较哪些位；同一规则的已选字段全部满足才命中。** RSSI 提供 RANGE 和 BITS 两种明确模式，不把负数位掩码解释成大小比较。

## 2. 新增需求与原编号追溯

下面是新增过滤专项编号，补充而不重排系统 93 条、ESP 182 条、平台 59 条既有需求。三方专项需求直接引用这些编号，不再分配内容重复的新编号。

| 编号 | 必须满足的要求 |
| --- | --- |
| BFLT-001 | NAME、IBEACON_UUID、RSSI、MAJOR、MINOR、SERVICE_UUID 可独立启用并同条 AND；拒绝空选择及未定义位。 |
| BFLT-002 | 名称按字节 value/mask 比较，明确完整/缩短名称来源及 EXACT/PREFIX，不隐含大小写折叠、正则或 Unicode 归一。 |
| BFLT-003 | iBeacon UUID 和 GATT 服务 UUID 是两类字段；两类均支持 16 字节掩码，服务 UUID 先按 AD 字节序归一。 |
| BFLT-004 | Major/Minor 按各 16 位数值 value/mask 比较，允许段过滤；识别 iBeacon 后才解释这些字段。 |
| BFLT-005 | RSSI RANGE 为有符号 dBm 闭区间，BITS 为同一 i8 补码的一字节位比较；未知 RSSI 不匹配任何已启用 RSSI 条件。 |
| BFLT-006 | 1～8 条有序规则首条命中生效；有 ALLOW 而无命中则拒绝，否则放行；不擅自改为任意条件 OR。 |
| BFLT-007 | REPORT 模式每个报告独立过滤；PAIR 模式必须显式启用、主动 legacy 扫描、固定窗口、有界缓存且一对一。 |
| BFLT-008 | 不跨 radio、boot、配置 revision、真实地址或地址类型拼接；缺响应、歧义、缓存满和超时不得绕过过滤。 |
| BFLT-009 | iBeacon UUID/Major/Minor 必须来自同一个合法 iBeacon AD；不跨多个厂商结构拼字段。 |
| BFLT-010 | 过滤通过仍逐条保留原始 ADV/SCAN_RSP、各自 RSSI/时间/序号；不得生成拼接 AD 或伪造共享采集时间。 |
| BFLT-011 | 旧规则及新规则不得同时激活；模式切换通过一次完整配置事务原子完成，不留无过滤空窗。 |
| BFLT-012 | 平台、ESP、nRF 使用同一编码/匹配向量；整配置超容量必须拒绝，不裁掉规则、字段或 mask。 |
| BFLT-013 | 只有签名清单、设备当前能力及相应驱动均支持时才能提交 V2；旧设备不得静默降级为近似过滤。 |
| BFLT-014 | 完整读回含引擎、scope、规则顺序、掩码、基础过滤、revision，恢复及幂等比较不能只看扫描参数。 |
| BFLT-015 | 缓存和计数有明确单位；配置切换/停扫/重启清理旧上下文并可解释丢弃。 |
| BFLT-016 | 平台编辑界面按字段生成规范配置，显示有效条件交集、掩码示意及离线预览；秘密/运行源码不进入该配置。 |
| BFLT-017 | 原有独立双射频配置保留，同一采集任务可分别下发相同过滤目标；不自动覆盖另一 radio。 |
| BFLT-018 | 过滤发生在跨射频去重之前；500 ms 名称配对、50/150 ms 跨射频合并和平台连接证据窗口是独立处理。 |

## 3. 平台配置合同

以下字段已同步 Schema/OpenAPI/源码；必须先部署兼容服务器及0.3.0固件，并通过目标能力门控，旧部署继续拒绝未知 `beaconFilterV2`。任务信封仍沿用既有 CONFIGURE 和 revision，不把所有消息的 schemaVersion 改成 2。

新增 `radios.nrf54.beaconFilterV2` 与 `radios.esp32.beaconFilterV2`，值为 `null` 或完整对象。对象/规则数组整体替换，不对子规则做不明确的合并。

| 字段 | 类型及约束 |
| --- | --- |
| `version` | 整数，固定 2 |
| `scope` | `REPORT` 或 `ADV_SCAN_RSP_PAIR` |
| `pairWindowMs` | PAIR 时必填且固定 500；REPORT 时禁止出现 |
| `rules` | 1～8 个有序规则；停用使用对象 null，不能用空规则伪造有效 V2 |
| 每条 `action` | `ALLOW` / `DENY` |
| 每条 `reportKinds` | REPORT 使用现有四类报告非空无重复集合；PAIR 必须为 `["LEGACY_ADV"]`，表示锚定 ADV，输出仍含两个原始种类 |
| 每条 `fieldMask` | JSON 整数 1～63；各 bit 与出现的谓词字段严格一致，未知/多余/缺失字段均拒绝 |

| bit / 数值 | 谓词字段 | 内容合同 |
| --- | --- | --- |
| 0 / 1 | `name` | `{mode,source,valueBase64,maskBase64}`；mode=`EXACT/PREFIX`；source=`COMPLETE/SHORTENED/ANY`；两串解码同长 1～29 字节，mask 非全0 |
| 1 / 2 | `ibeaconUuid` | `{value,maskHex}`；value 为规范小写128位UUID；maskHex 为32个小写hex字符且非全0，不按UUID版本/variant约束mask |
| 2 / 4 | `rssi` | RANGE：`{mode:"RANGE",minDbm,maxDbm}`，两端必填、整数且 -127≤min≤max≤20；BITS：`{mode:"BITS",valueDbm,mask}`，valueDbm 为 -127～20 整数、mask 为1～255整数 |
| 3 / 8 | `major` | `{value,mask}`；value 为0～65535整数，mask 为1～65535整数 |
| 4 / 16 | `minor` | 同 major |
| 5 / 32 | `serviceUuid` | 同 ibeaconUuid 的规范128位value及16字节mask；原16/32位服务UUID先展开Bluetooth Base UUID |

所有对象拒绝未知字段；数值不接受字符串、浮点、负mask或超范围值。mask=0 的字段应通过清除 fieldMask 对应位关闭；不允许零mask ALLOW 意外吞掉后续规则。配置保留所填 value 的未选位，不在回显中擅自改写；比较只使用mask所选位。

基础 `filters` 仍在 V2 前执行。REPORT 保持原逐报告语义；PAIR 在已形成的报告对上按类别 AND 评估：RSSI 用锚定 ADV，serviceUuids/manufacturerIds 可从任一完整 AD 获取，各列表内部仍 OR。基础 minRssi 与规则 RSSI 同时启用时取交集，界面必须显示，不能隐式清除基础过滤。

### 3.1 原子切换与能力

- 旧 → V2：若旧规则非空，同一补丁显式包含 `beaconRules:[]` 和新 V2 对象。
- V2 → 旧：同一补丁显式包含 `beaconFilterV2:null` 和目标 `beaconRules`；两项同属一个 radio 的扫描组。
- 合并后的配置不得同时有非空旧规则和非null V2。平台、ESP 都检查最终对象，不能先停用后另发任务启用。
- 平台能力字符串为 `NRF54_BLE_FILTER_V2` / `ESP32_BLE_FILTER_V2`，按目标 radio 授权；nRF HELLO为 bit13；ESP 本机驱动独立能力。cap13意味着完整实现本规范 REPORT/PAIR及读回/计数，不是仅支持一个字段。
- 无能力的设备继续使用旧配置；V2请求明确拒绝，不生成会改变语义的近似旧规则。新版平台仍接受旧协议，旧平台只能下发旧配置。

## 4. 字段解析与掩码

共同公式：`(actual[i] & mask[i]) == (value[i] & mask[i])`，参与字段的所有字节均相等才通过。Major/Minor等价使用u16公式。不存在的合法字段视为该谓词不命中；DENY清单不是白名单，若要求字段必须存在，应配置包含这些字段的ALLOW规则。

### 4.1 名称

Local Name 的 AD type 为 `0x09` Complete / `0x08` Shortened。value由UI的UTF-8文本编码或高级原始字节输入生成，掩码作用于字节，不是字符位置。大小写敏感，不做Unicode归一或正则。EXACT要求源Data长度恰为value长度，PREFIX允许更长。

UI文本入口严格编码UTF-8；高级valueBase64/maskBase64及收到的名称按原始字节处理，不要求被掩掉的位或单独前缀能解码成完整字符，不做替换字符修复。字段长度始终是解码后字节数。

COMPLETE只选0x09；SHORTENED只选0x08；ANY存在完整名称时优先0x09，否则0x08。选择来源后，重复相同Data可视为一个候选；多个相互冲突的同类名称不能任取其一，按上下文歧义拒绝。规则明确请求SHORTENED时不会被0x09替代。不得把前几个字节相同的缩短名称宣称为完整名称。

### 4.2 iBeacon UUID、Major、Minor

只在 `AD type=0xFF` 且 Data **恰25字节**、前4字节为 `4C 00 02 15` 时解释以下字段；该结构识别不可被字段mask掩掉。自定义厂商格式继续使用旧原始规则或未来另行声明的解析格式。

| Data偏移 | 长度 | 含义 / 比较顺序 |
| --- | --- | --- |
| 0 | 2 | Company ID `4C 00`，公司标识为小端 |
| 2 | 2 | iBeacon 类型及长度 `02 15` |
| 4 | 16 | iBeacon UUID，按规范UUID文本从左到右字节比较，不整串反转 |
| 20 | 2 | Major，空口大端，转u16后比较mask |
| 22 | 2 | Minor，空口大端，转u16后比较mask |
| 24 | 1 | measured power，1m校准RSSI；不是本次接收RSSI |

同条规则选择的 iBeacon UUID/Major/Minor 必须由同一个该格式的完整AD同时满足；多个厂商结构不能分别贡献部分字段。Major=`0x1200`、mask=`0xFF00`匹配`0x12xx`；Minor=`3`、mask=`0x000F`匹配低4位为3。平台JSON用十进制数；UART通用u16仍按小端传输，不能因iBeacon空口是大端而更改UART整数约定。

### 4.3 服务 UUID

从服务列表AD `0x02..0x07`、Service Data `0x16/0x20/0x21`读取，先按其空口字节序归一为128位，再应用mask。至少一个完整服务UUID满足即通过该谓词。iBeacon厂商UUID不冒充服务UUID；自动连接仍按既有服务解析、授权和新鲜ADV证据执行，过滤命中不自动授予连接权限。

### 4.4 RSSI

- RANGE：`minDbm <= measuredRssi <= maxDbm`，包含端点。仅需“≥-75 dBm”可配置[-75,20]。
- BITS：先排除未知/不在-127～20范围的观测，再把 actual/valueDbm转为8位补码应用mask；绝不是按uint8大小比较。
- 示例 valueDbm=-64（`C0`）、mask=240（`F0`）命中`C0..CF`，即-64～-49 dBm；不会等价于“≥-64”。UI显示这个区别，不默认启用BITS。
- RSSI=`-128`为内部未知哨兵，不允许作为平台阈值或value。若基础或任一V2规则启用RSSI，未知值使该上下文拒绝并计数，不能被后续DENY默认放行绕过。
- REPORT用本报告RSSI；PAIR只用ADV的接收RSSI。SCAN_RSP原RSSI仍原样上报，不能拿iBeacon measured power替代。

## 5. REPORT 与名称配对

REPORT没有等待，每个ADV/SCAN_RSP独立解析与判断，不从历史设备缓存借名称。旧规则也始终保持逐报告语义。

PAIR是显式新模式，用于名称在SCAN_RSP而UUID/Major/Minor在ADV的场景：

1. 必须主动legacy扫描，rule.reportKinds固定LEGACY_ADV；扩展报告不在该模式支持集，按unsupported_context拒绝。未知真实地址不关联。
2. 每个radio独立缓存，键含radio、controllerBootId、scanRevision、addressType和真实空中address。不能按bond identity替代地址，不能跨芯片、随机地址变化或重启匹配。
3. 只考虑SCAN_RSP之前且时间差≤500ms的尚未配对ADV；恰有一个候选才关联。一条响应不得配多条ADV；零候选响应计pair_ambiguous丢弃，多个候选时响应及相关待配ADV全部计pair_ambiguous丢弃，不选“最近者”冒充唯一证据。
4. 这是有限时间内的上下文关联，不是协议层提供的空口交易ID证明；若业务要求确定的同次请求身份，需另有设备业务协议，本过滤器不能伪造。
5. 每radio最多32个待配ADV，缓存预算4096B包含元数据和拷贝；先到任一限制即拒绝新入报告并计溢出，不驱逐旧条目冒充成功。等待使用本radio单调时间；BLE回调只做有界复制，worker判定，不阻塞500ms。
6. 超过500ms确认无响应、停止扫描、配置切换或身份变化时，未判定ADV不得按仅DENY的默认动作放行。时间差取原始回调入队时的单调capture_ms，不取worker处理时刻。capture_ms恰等于ADV+500的响应仍优先配对；worker先处理已入队时间≤deadline的报告，只有生产者水位严格大于deadline且此前报告已处理完才发超时。时间戳分配/入队发布与水位推进须串行化，防止较早时间戳的报告在超时后才发布；无流量时通过同一队列的计时水位推进，不能永久等待。迟到超过deadline的响应不复活已过期ADV，计零候选歧义；队列满时新报告计pair_overflow，不能无界延长缓存。有效完整报告对中字段确实不存在，则按谓词false及正常规则顺序处理。
7. 形成完整上下文后执行基础过滤和有序V2规则。放行时先输出原ADV再输出原SCAN_RSP，两条保留原始字节、时间、RSSI及不同序号；不创建合成广播。UART CUSTODY在后续ESP持久接管时才发生，配对缓存只是易失内存。
8. 配置失败回退时恢复旧引擎/完整目标，清掉切换期间待配上下文，不能让旧revision的数据被新规则解释。控制器重启丢失未上报缓存是明确故障边界。

V2对AD边界严格校验；零长度终止后的填充只能为0。格式畸形拒绝并计数，旧模式无内容过滤时保留畸形原始AD的诊断行为不变。

## 6. UART 与 ESP 内部表示

使用能力bit13、`0x19 SET_SCAN_PROFILE_V2`、`0x1A GET_SCAN_PROFILE_V2`、`0x1B GET_SCAN_FILTER_STATUS`；线上frame仍1，COBS/CRC32C及3072字节payload上限不变。精确偏移和枚举以[UART扩展](../server/docs/gateway-uart-protocol.md)为准。

完整body按顺序为：`format_version:u8=2, engine:u8, legacy_len:u16, legacy_profile[legacy_len], scope:u8, pair_window_ms:u16, rule_count_v2:u8, rules_v2[]`。旧ScanProfile完整嵌套，使新旧切换仍能原子设置基础过滤与旧规则。engine=0为LEGACY且scope/window/count为0；engine=1为V2且旧rule_count=0、新rule_count为1～8。

每条包含 `rule_len:u16`（不含自身）、`action:u8,kind_mask:u8,field_mask:u16`，再按bit升序写入已选字段。名称3+2N字节，UUID各32字节，RSSI3字节，Major/Minor各4字节；不允许省略已启用字段或携带尾随未知数据。

body最大 **3070字节**，为响应opcode/status保留2字节。body长度=`8+legacy_len+Σ(2+rule_len)`；最长单规则142B。V2所有局部上限同时取满会超过总帧预算：8+2323+8×142=3467，应整项拒绝。旧完整profile最大2755，包装后2763，仍可无损承载。平台预检、ESP编码、nRF解码三者须使用同一长度公式，不截短名单。

V2活动时旧SET_SCAN_CONFIG/SET_SCAN_PROFILE和GET_SCAN_PROFILE返回CONFLICT；GET_SCAN_CONFIG只能读基础子集，不可用于完整恢复。新GET总返回完整wrapper；退回旧模式通过新SET的engine=0完成，不要求先清空过滤。相同revision/相同完整body幂等、同revision不同body冲突、旧revision拒绝。

Central/GATT、广播互斥、网络维护及双组件OTA中的暂停/恢复路径也必须识别当前过滤引擎，保存并恢复完整wrapper；不能为临时停扫调用旧SET而清掉V2。Central/DFU已有控制器内部暂停及ESP本机暂停路径须保留目标revision和完整引擎；其他确需变更目标的停扫走正式CONFIGURE事务及完整V2编码，不假设已有通用UART暂停命令，也不得凭空给无云任务的暂停分配平台revision。恢复后清旧PAIR上下文，重新采样，不复用维护前名称。固件降级到不支持V2的版本前须完成受授权的旧目标迁移；无法无损转换时明确阻止，不自动弱化条件。

0x1B提供96字节状态：当前revision/u64、engine/u8、scope/u8、pending_count/u16、pending_bytes/u32；随后10个u64依序为 records_seen、records_passed、rule_dropped、pair_timeout、pair_ambiguous、pair_overflow、malformed_ad、unknown_rssi、unsupported_context、config_discarded。

pending_bytes仅累计待判原始AD字节，不含头、索引与实现元数据；4096B是另一个实际总内存预算，不能拿pending_bytes代替内存占用。engine=0沿用旧匹配行为，不启用V2的严格畸形AD/未知值处理；新状态查询不能改变旧过滤语义。

计数单位均为原始报告条数，同boot累计、不随配置清零；PAIR放行/规则拒绝各计2条，每条seen一次且只有一个过滤终态原因，待配条目尚无终态。优先归因unsupported_context→malformed_ad→unknown_rssi→配对故障→规则拒绝。records_passed只表示过滤器输出资格，后续FIFO满由既有dropped_full记录，不证明持久接管。新计数将置于正式扩展的BLE诊断结果，不能直接塞进当前严格Schema。

## 7. 完整配置示例与预期

这是新Schema上线后的扫描组补丁示例；不是目前可发送的请求。fieldMask=31选中前五项，不选服务UUID。实际任务仍需完整身份、权限、revision及幂等参数。

```json
{
  "radios": {
    "nrf54": {
      "scan": {"enabled": true, "active": true, "intervalMs": 100, "windowMs": 50},
      "filters": {"minRssi": null, "serviceUuids": [], "manufacturerIds": []},
      "beaconRules": [],
      "beaconFilterV2": {
        "version": 2,
        "scope": "ADV_SCAN_RSP_PAIR",
        "pairWindowMs": 500,
        "rules": [{
          "action": "ALLOW", "reportKinds": ["LEGACY_ADV"], "fieldMask": 31,
          "name": {"mode": "PREFIX", "source": "COMPLETE", "valueBase64": "QmNuLQ==", "maskBase64": "/////w=="},
          "ibeaconUuid": {"value": "fda50693-a4e2-4fb1-afcf-c6eb07647825", "maskHex": "ffffffffffffffffffffffffffffffff"},
          "rssi": {"mode": "RANGE", "minDbm": -75, "maxDbm": -35},
          "major": {"value": 4608, "mask": 65280},
          "minor": {"value": 3, "mask": 15}
        }]
      }
    }
  }
}
```

输入ADV十六进制：`0201061aff4c000215fda50693a4e24fb1afcfc6eb0764782512340003c5`；SCAN_RSP：`070942636e2d3031`（名称Bcn-01）；ADV RSSI=-60，响应在100ms后且候选唯一时放行两条。`C5`是iBeacon校准值，不影响接收RSSI判断。

同一例以示例revision=1编码：ScanConfig为16B，嵌套旧ScanProfile为19B，V2规则体为58B，完整wrapper为87B。以下是连续wrapper十六进制；不包含opcode、status、帧头、CRC或COBS。SET payload为88B，成功响应payload为89B，均未超预算。

```text
020113001000010000000000000003a00050008000000001f401013a0001011f0001000442636e2dfffffffffda50693a4e24fb1afcfc6eb07647825ffffffffffffffffffffffffffffffff00b5dd001200ff03000f00
```

此向量已按JSON/Base64、iBeacon偏移、大端Major/Minor、RSSI补码及UART小端长度独立计算核对；这是文档示例校核，不是V2固件运行测试。实际revision仍由平台分配，不能在实现中写死1。

## 8. 跨端一致性向量与验收

以下向量由平台预览器、ESP本机引擎和nRF共享C过滤器使用同一输入。0.3.0已增加生产C引擎、配置编译器和平台预览测试；空口行为仍需实板验收。

| 用例 | 输入/变化 | 必须结果 |
| --- | --- | --- |
| BFLT-AT-01 | 上述完整示例，响应+100ms，ADV=-60、RSP=-90 | 放行原ADV和RSP；RSSI判断取ADV，内容/时间不改 |
| BFLT-AT-02 | Major=0x12FF、Minor=0x00A3；再改Major=0x1300或Minor=0x0004 | 前者通过，两个后者分别拒绝 |
| BFLT-AT-03 | UUID mask只保留前4字节，后12字节变化；选中位变化 | 未选位变化通过，选中位变化拒绝；禁止UUID整串反转 |
| BFLT-AT-04 | PREFIX Bcn-匹配Bcn-01；EXACT同pattern匹配Bcn-01 | 前者通过，后者拒绝；bcn-01大小写不同拒绝 |
| BFLT-AT-05 | 名称仅0x08，source COMPLETE/SHORTENED/ANY | COMPLETE不命中，后两者按字节匹配；冲突完整名称上下文拒绝 |
| BFLT-AT-06 | RANGE[-75,-35]：-76/-75/-35/-34/-128 | 依序拒绝/通过/通过/拒绝/未知拒绝 |
| BFLT-AT-07 | BITS(-64,240)：-65/-64/-49/-48/-128 | 依序拒绝/通过/通过/拒绝/未知拒绝 |
| BFLT-AT-08 | 两条ALLOW分别匹配名称和UUID；单条选二字段 | 验证OR式规则选择与单条AND不同；UI不可把前者生成成后者需求 |
| BFLT-AT-09 | 同radio唯一ADV+RSP恰500ms/501ms；另radio/boot/address/revision | 500ms可配；501ms过期；所有跨上下文拒绝，不跨radio缓存 |
| BFLT-AT-10 | 同地址两个未配ADV后一个RSP；仅DENY规则下无RSP | 歧义全部不放行；超时也不因DENY默认放行 |
| BFLT-AT-11 | 基础minRssi=-50、规则RANGE[-75,-35]，ADV=-60 | 拒绝，界面显示交集[-50,-35]，不自动删除基础阈值 |
| BFLT-AT-12 | 16/32/128服务UUID归一；相同字节放入iBeacon UUID | 服务谓词前者等价匹配；后者不能冒充服务；多个FF不能拼Major/UUID |
| BFLT-AT-13 | 零mask、错长度、fieldMask不符、未知位、body3071B | 平台/ESP/nRF一致拒绝，旧配置不变；3070B边界合法内容可编码 |
| BFLT-AT-14 | legacy↔V2原子切换，重复revision、旧revision、驱动应用失败 | 无无过滤空窗；幂等/冲突一致；失败恢复旧完整引擎且清缓存 |
| BFLT-AT-15 | 第33个待配ADV或4096B预算耗尽、配置切换/重启 | 有界拒绝和正确原因计数；无无限分配；重启不得复用旧名称 |
| BFLT-AT-16 | 旧平台/设备，混合能力两radio，回读后重启恢复 | 旧功能保持；V2显式能力拒绝；完整wrapper恢复，不近似降级 |

开发闭合条件：先同步OpenAPI/Schema/能力清单/平台编辑与预览，再实现共享解析匹配、nRF命令及ESP适配，运行上述向量、故障测试和两端构建，最后用真实Beacon空口验证名称配对与RSSI。文档统一不是功能发布完成。
