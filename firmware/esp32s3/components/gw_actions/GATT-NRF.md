# nRF54L15 GATT执行器

`gw_gatt_nrf.c`按[UART协议](../../../../server/docs/gateway-uart-protocol.md)发送Central动作，按[平台协议](../../../../server/docs/gateway-protocol.md)输出任务结果。它不实现另一份蓝牙栈。

## 已实现的任务

- `GATT_CONNECT`：真实地址连接、实际地址/类型/安全状态/MTU回读、规范连接引用登记。
- 自动连接：核对capture组件保留的nRF启动ID、事件ID、真实地址与新鲜可连接ADV证据；查找服务、按需发现特征，再核验实际连接。
- `GATT_READ/WRITE`：无引用时在同一任务中连接、发现、读写、关闭；有引用时核对已登记活连接与任务目标。写入只支持WITH_RESPONSE，READBACK在同一连接读取并比较。
- `GATT_SUBSCRIBE/UNSUBSCRIBE`：发现当前连接的特征/CCC、核对属性、等待nRF终态。通知数据由capture组件独立接管。
- `GATT_DISCONNECT`：等待断开终态并回读连接不存在；断开事件持久提交后更新本地登记。

`BONDED_ENCRYPTED`仅调用nRF已有绑定加密路径，不触发首次配对。能力位2/9缺失时拒绝Central任务，订阅还需位3。所有步骤共用任务有效期和100～30000ms预算。

## 持久化与恢复

每个动作先在`gatt/nrf-operation`保存nRF启动ID、非零递增op_seq、精确请求字节、taskId和INTENT，成功提交后才发送UART。立即ACK仅表示受理，随后每100ms查询GET_OP_RESULT，终态持久化后才能分配下一序号。

超时、错误回显、控制器重启或UNKNOWN均不得重发物理写。相同启动周期内未解决的操作阻止新动作；上层actions/cloud还保留整个任务的请求摘要和UNKNOWN状态。一个多步骤任务只完成部分步骤时，不能由最后一次UART成功推断整任务成功。

最终任务结果存于`gatt/nrf-result`。长连接登记与完整CONNECT结果通过同一gw_store事务发布；`gw_gatt_nrf_recover`仅回放同taskId的完整持久结果。上层负责验证请求摘要。终态前掉电且没有完整结果时返回UNKNOWN，保留诊断和维护占用，等待对账；当前没有自动解除不确定写入的管理入口。

每2秒查询连接。nRF启动ID变化或连接消失产生持久GATT_LINK_DOWN。ESP重启先对账旧op，再在维护互斥下关闭nRF遗留连接；也处理CONNECT成功后尚未登记即掉电的窗口。事件由capture根据连接引用和原因给出稳定身份，因此重复提交不会产生第二个业务事实。

## 主机验证

运行`firmware/esp32s3/scripts/Test-Gatt.ps1 -Compiler <C编译器或zig> -CjsonDir <cJSON源码目录>`。测试直接编译生产执行器，替换UART、Flash、时钟及系统调用边界，覆盖：意图先落盘、序号单调、Flash失败零发送、丢ACK/UNKNOWN不重复写、严格UUID/MAC、READBACK与清理、新鲜观测拒绝、CONNECT登记和完整结果原子发布。

主机测试不证明UART时序、无线配对、真实CCC、NVS掉电或硬件吞吐；目标编译及实板验证状态以工程验证文档为准。
