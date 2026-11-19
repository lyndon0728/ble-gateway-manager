# HTTPS 设备传输与绝对截止时间

`gw_http_json` 和 `gw_http_stream` 保持原公开接口。网络探测、任务结果、凭据领取和原始事件上传都使用相同的认证、候选接口绑定及截止时间实现。

## 传输实现

本组件先在指定接口上发起新的 UDP DNS 查询，绑定接口与源 IPv4 地址；随后连接解析得到的 IPv4 地址。TLS 使用制造区 CA，`common_name` 保持制造配置中的原始主机名，保留证书主机名校验和 SNI；HTTP Host 同样使用原始主机名及端口。请求只发送到制造配置的 api_url 或单独的 ingest_url，不跟随重定向。

构建强制 `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF=y`。ESP-IDF v5.5.6 默认关闭此选项；关闭时 `esp_netif_get_dns_info(netif, ...)` 仍返回全局 DNS，不能证明候选接口的 DNS 配置正确。开启后读取该接口保存的 DNS，避免 Wi-Fi 与以太网同时启用时互相冒充探测结果。

HTTPS 使用 `esp_tls_conn_new_async`、`esp_tls_conn_read` 和 `esp_tls_conn_write`。`non_block=true` 保持底层 socket 非阻塞；TCP 建立的 select 使用剩余期限且最多 10 秒，握手及后续 WANT_READ/WANT_WRITE 由当前任务短暂让出后继续。每次连接、读、写之前和之后均检查绝对单调截止时间；截止、长度超限、解析失败或 sink 失败后立即退出，由同一任务销毁 TLS 对象。没有访问客户端对象的跨线程定时器。

固定 SDK 的异步连接在 TCP select 超时后保留被清空的 fd_set。返回 0 时通过公开 `esp_tls_get_conn_state` 区分 CONNECTING 和 HANDSHAKE：前者表示 TCP 等待已结束，直接中止，不能在空描述符集合上反复等待；后者才继续推进握手。主机测试单独覆盖这一分支。

原来的 `esp_http_client_perform` 回调返回错误不能作为中断依据。已核对固定 SDK v5.5.6 的 `components/esp_http_client/esp_http_client.c`：`http_on_body` 忽略 `http_dispatch_event(HTTP_EVENT_ON_DATA)` 的返回值；`perform` 在持续可读的 chunked body 内循环也可能不向调用者返回。仅改成 async perform 并在外层计时，仍不能处理这种持续流。因此本组件采用自己控制的有界读取循环。

## HTTP 消息边界

请求使用 HTTP/1.1、`Connection: close`、`Accept-Encoding: identity` 和准确的 Content-Length。响应支持 Content-Length、chunked 及关闭连接结束的合法消息。仅接受 2xx，不处理重定向；拒绝重复 Content-Length、Content-Length 与 Transfer-Encoding 同时存在、不支持的编码和提前 EOF。

| 限制 | 上限 |
| --- | --- |
| 响应头总字节数 | 8192 字节 |
| 单行响应头 | 1024 字节 |
| 单个 chunk 长度行及扩展 | 256 字节 |
| trailers 总字节数 | 8192 字节 |
| 一次 TLS 预读 | 2048 字节 |
| JSON 响应正文 | 128 KiB |
| 制品流正文 | 调用者提供的签名清单/分区限制 |

已知 Content-Length 超过上限时，不向 sink 交付任何正文。chunked 累计超过上限时，不交付超出上限的块。sink 首次失败后不再调用 sink，也不再发起后续 TLS 读取；已预读到本地的一个 TLS 缓冲区随后清除。

传输暂存区、HTTP 预读区、请求凭据及序列化请求在释放前显式清零，JSON 接收区扩容使用分配/复制/清除旧区，避免 realloc 留下旧密码副本。成功解析后返回的 cJSON 由调用者拥有；凭据领取调用者仍负责清除密码字段。解析和协议限制不代替制造凭据保护或 TLS 验证。

## 主机测试

```bash
python firmware/esp32s3/components/gw_http/tests/run_host_tests.py \
  --cc gcc --idf "$IDF_PATH"
```

Windows 可将 `--cc` 指向 Zig 可执行文件；测试程序需要 ESP-IDF 源码中的 cJSON。纯 C framing 测试涵盖逐字节拆分、chunk 扩展/trailers、总头长度、行长度、提前 EOF、重复/冲突长度和超限。适配器测试直接编译生产 `gw_http.c`，仅替换 SDK TLS、接口、DNS和时钟 I/O，验证持续可读 chunk 在绝对期限停止、连接/写/读等待期限、sink 失败后停止、单次对象销毁，以及 Host/SNI/CA/指定接口参数。

这些测试证明程序的边界和停止控制；真实 TLS 握手、板上调度时延、DNS服务器以及候选路由切换仍需实板联调。目标构建与实板状态以[固件验证记录](../../VALIDATION.md)为准。
