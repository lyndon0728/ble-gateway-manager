# 客户字段映射

目标配置的 `mappingVersion` 与 `mapping` 一起冻结在目标版本和规则包中。映射只改变输出消息的 `data`，不会改写 `eventId`、`recordKey`、`processingRevision`、`deliveryId` 或原始数据。目标创建后修改映射需要发布目标新版本，并激活引用新版本的规则包；旧事件和投递重试继续使用原版本。

无自定义映射时使用 `mappingVersion: identity-1` 或 `1`，完整保留标准化字段。自定义版本必须携带定义，例如：

```json
{
  "mappingVersion": "customer-temperature-v1",
  "mapping": {
    "fields": [
      {"source":"temperature","target":"tempF","scale":1.8,"offset":32,"unit":"degF"},
      {"source":"batteryMillivolts","target":"batteryV","scale":0.001,"unit":"V"}
    ],
    "constants": {"source":"own-ble-gateway"},
    "wrapKey": "measurements"
  }
}
```

这部分配置与 `protocol`、`endpoint`、ACK 模式、批量及额度等字段一起提交到 `POST /api/v1/targets` 的 `configuration`，或目标版本创建接口。规则中目标引用的 `mappingVersion` 必须相同。

支持 1–64 个顶层字段，数值计算为 `值 × scale + offset`；`unit` 会生成 `<target>Unit` 字段。自定义映射仅输出选定字段和至多 32 个标量常量；`wrapKey` 可将这些字段包装为一个对象。单位换算由配置明确给出，服务器不会根据单位名称猜测换算。数值使用 IEEE 754 双精度，客户展示可按自己的精度格式化。

源字段缺失、数值类型错误、输出字段冲突或数值溢出会使本次处理进入隔离区，原始报文保留。服务器不会提交一部分客户输出后再忽略另一部分错误。映射没有脚本、表达式执行、文件访问或网络访问能力。

标准化处理结果与各目标的最终传输内容分开保存。目标映射后的字节写入持久 outbox 后才开始发送，因此修改配置或服务重启不会改变已经生成的重试内容。
