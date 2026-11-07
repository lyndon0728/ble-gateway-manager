# 参考传感帧 reference-sensor-v1

这是为本项目新设计的传感数据格式，可承载 BLE 广播中的厂商自定义数据或网关归一后的原始采样数据。它不是某个已量产型号的既有协议，也没有分配 Bluetooth SIG Company Identifier。真实 BLE AD 中需要合法厂商 ID 时，应使用所属公司的已分配 ID；不能把 `BG` 当作厂商注册号。

服务器解析器 `reference_sensor`，版本 `1.0.0`，输出 schema `reference-sensor-1`。`protocolHint` 使用 `reference-sensor-v1`。输入为**仅参考帧本身**的 18 字节；完整 BLE AD 应先按 `ble_ad` 解析结构，不能不区分封装直接传给此解析器。

| offset | 字节数 | 字段 | 编码/范围 |
| --- | --- | --- | --- |
| 0 | 2 | magic | ASCII BG，十六进制 `42 47` |
| 2 | 1 | version | `01` |
| 3 | 1 | type | `01`，温湿度采样 |
| 4 | 4 | sampleSequence | uint32，大端；传感器自己的采样序号 |
| 8 | 2 | temperature | int16，大端；除以 100 得摄氏度；原始范围 -5000..12500 |
| 10 | 2 | humidity | uint16，大端；除以 100 得相对湿度%；0..10000 |
| 12 | 2 | batteryMillivolts | uint16，大端；0..6000 mV |
| 14 | 2 | flags | uint16，大端；bit0=sensorFault，其他位必须 0 |
| 16 | 2 | CRC | CRC16-CCITT-FALSE，大端，覆盖 offset0..15 |

CRC 参数：poly=0x1021，init=0xFFFF，refin=false，refout=false，xorout=0。标准校验输入 ASCII `123456789` 的结果应为 `0x29B1`。CRC 仅用于识别数据损坏，不提供身份认证或防伪。

模拟器生成 sampleSequence=1、25.00°C、50.00%、3000mV、无故障时，CRC 前的 16 字节为：

```text
42 47 01 01 00 00 00 01 09 C4 13 88 0B B8 00 00
```

解析结果字段：`temperature`、`temperatureUnit:"Cel"`、`humidity`、`humidityUnit:"%"`、`batteryMillivolts`、`sampleSequence`、`sensorFault`。长度、magic、版本、类型、范围、保留位或 CRC 任何一项不合法均拒绝解析并进入隔离流程，不输出看似正常的数值。

网关级 `eventId` 与传感器 sampleSequence 用途不同：eventId 是平台可靠传输/去重身份，sampleSequence 是业务采样数据。传感器重启、序号回绕、多个网关观察同一广播都不能以 sampleSequence 代替 eventId。

另提供两个测试解析器：`demo_sensor@1.0.0` 接受 4 字节大端 int16 温度/100 + uint16 湿度/100，输出 `temperature-1`；`ble_ad@1.0.0` 严格解析 BLE AD 长度字段，输出 `structures:[{type,dataBase64}]`、schema `ble-ad-1`，零终止符后只接受零填充。它们均不代表第三方设备型号已经适配。
