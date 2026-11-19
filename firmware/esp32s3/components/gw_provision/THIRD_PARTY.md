# 本地配网页面第三方来源

所有运行依赖随固件离线分发，不在用户浏览器中访问以下下载地址。

| 部分 | 锁定来源 | 许可 / 调整 |
| --- | --- | --- |
| SHA-512及内部工具 | [@noble/hashes 1.8.0](https://registry.npmjs.org/@noble/hashes/-/hashes-1.8.0.tgz)；[上游源码](https://github.com/paulmillr/noble-hashes/tree/1.8.0) | [MIT许可](web/vendor/hashes/LICENSE.md)；仅保留传递依赖，utils的包名crypto导入改为同目录`./crypto.js`以便离线加载，不改算法 |
| AES-256-GCM及内部工具 | [@noble/ciphers 1.3.0](https://registry.npmjs.org/@noble/ciphers/-/ciphers-1.3.0.tgz)；[上游源码](https://github.com/paulmillr/noble-ciphers/tree/1.3.0) | [MIT许可](web/vendor/ciphers/LICENSE.md)；只保留aes/_polyval/utils传递依赖，算法原样 |
| 浏览器SRP适配及Python参考 | [ESP-IDF v5.5.6 srp6a.py](https://github.com/espressif/esp-idf/blob/v5.5.6/tools/esp_prov/security/srp6a.py)、[设备C实现](https://github.com/espressif/esp-idf/blob/v5.5.6/components/protocomm/src/crypto/srp6a/esp_srp.c) | [Apache-2.0许可](tests/reference/LICENSE.md)；保留出处，客户端适配BigInt与Noble，前导零处理按设备C行为 |
| Protobuf布局与nonce时序 | [session.proto](https://github.com/espressif/esp-idf/blob/v5.5.6/components/protocomm/proto/session.proto)、[sec2.proto](https://github.com/espressif/esp-idf/blob/v5.5.6/components/protocomm/proto/sec2.proto)、[security2.py](https://github.com/espressif/esp-idf/blob/v5.5.6/tools/esp_prov/security/security2.py) | Apache-2.0；客户端只处理该有限字段集，设备仍使用SDK原生protocomm |
| 打包器 | [esbuild 0.25.5](https://github.com/evanw/esbuild/tree/v0.25.5) | MIT；只用于开发，不把工具二进制提交或装入固件 |

`tests/reference/sources.json`记录实际查阅参考文件的SHA-256。参考Python文件原样保留，字节转换shim独立列出；测试数据是公开固定fixture，不能作为生产随机数或默认密码。

实际下载包SHA-256：

| 制品 | SHA-256 |
| --- | --- |
| hashes-1.8.0.tgz | `e8a765d92c04faaccba8776411c5038cb195f812ee629fce07e1d2e6aec80ea0` |
| ciphers-1.3.0.tgz | `e02e7c03c26b8dd88ab0bd5e46f2996689e33f252bacb551a1cf504bc912501f` |
| esbuild win32-x64-0.25.5.tgz | `52c37fb9aab0dac0bef9fd829821df978db59053b7f6aa1adc3bdba32c216bc9` |

导入的Noble文件保留原版权和说明，换行统一LF并移除两行注释末尾空白；`package.json`仅标记该本地副本为ES module，不声称是完整npm包。静态bundle内保留上游合法注释。JS BigInt的时序和垃圾回收不具有专用硬件恒定时间/可靠擦除保证；该浏览器客户端的威胁边界须随项目发布评审及真实浏览器测试记录。
