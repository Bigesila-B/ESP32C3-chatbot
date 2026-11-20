# C3 工具调用轮次的内存瓶颈（未解决）

> 写给接手的模型/开发者：这是一个 **ESP32-C3 上 TLS 峰值内存不足** 的问题，不是 Wi-Fi、不是服务端。
> 聊天已经稳定；**只有带工具调用的轮次（尤其是 `web_search`）在第二次 LLM 请求时会握手失败**。
> 本文包含可复现步骤、实测数据、已排除的方向和约束，请先读完再动代码。

## 环境

| 项 | 值 |
|---|---|
| 芯片 | ESP32-C3 (QFN32) rev v0.4，内置 4 MB Flash，**无 PSRAM** |
| 框架 | ESP-IDF 5.5，`CONFIG_APP_CLAW_TEXT_ONLY=y`，`MEMORY_MODE_LIGHTWEIGHT` |
| 固件 | 分支 `feature/context-compaction`，提交 `cfaf436` |
| 启动后堆 | 空闲约 71–75 KB，最大连续块 51–57 KB |
| Wi-Fi | STA 连 2.4 GHz，RSSI −37 ~ −56 dBm；APSTA 模式（配网热点常开） |
| 模型接口 | `https://apihub.agnes-ai.com/v1`，`agnes-3.0-flash`，`llm_timeout_ms=120000` |
| 串口 | 原生 USB-Serial/JTAG，115200 |

## 现象

普通对话正常（实测连续 5 次全部成功，5–7 秒）。一旦模型调用 `web_search`：

1. 第一次 LLM 请求成功，工具执行成功（`tool_result ... err=ESP_OK`）
2. **第二次 LLM 请求在 TLS 握手阶段失败**，前端收到 `HTTP request failed: ESP_ERR_HTTP_CONNECT`
3. 串口日志每约 5 秒重复：

```
E (157798) Dynamic Impl: alloc(5045 bytes) failed
E (162884) Dynamic Impl: alloc(5045 bytes) failed
...
```

4. 约 120 秒后 LLM 超时，请求结束，堆回到 70 KB。

失败时抓到的现场：

| 指标 | 数值 |
|---|---|
| 第二次请求体 | 10 114 – 11 245 字节 |
| `esp_get_free_heap_size()` | 32 944 – 35 808 |
| `heap_caps_get_largest_free_block()` | 9 728 – 21 504 |
| 失败的分配 | 5 045 字节 |
| 请求结束后 | 空闲 70 KB、最大块 55 KB |

**结论：这是峰值/碎片问题，不是泄漏。** 结束后内存完整归还，但握手瞬间连续块不够。

## 一次请求里的内存构成

| 占用项 | 大小（字节） | 来源 |
|---|---|---|
| 人格（system prompt） | 4 934 | `/memory/soul.md` |
| 工具清单（名称+描述+schema） | 1 477 | `cap_web_search`、`memory_remember`、系统类工具 |
| 会话历史（已裁剪到最近 3 轮 user） | 2 550 – 2 743 | `claw_memory_session_history_collect()` |
| 请求 JSON 其余部分 | ~1 000 | 消息结构、模型参数 |
| TLS 接收缓冲 | 8 192 | `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` |
| TLS 发送缓冲 | 2 048 | `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` |
| 证书链校验临时缓冲 | ~5 045 | 失败的那次分配 |
| HTTP 收发缓冲 | 2 048 × 2 | `claw_llm_http_transport.c` |

## 已经试过并提交的改动

提交历史（都在 `feature/context-compaction` 上）：

| 提交 | 内容 | 效果 |
|---|---|---|
| `1581cda` | 去掉请求体整份复制做 UTF-8 清洗；HTTP 缓冲 4 KB→2 KB；TLS 发送记录 4 KB→2 KB | 修好了普通对话的 `ESP_ERR_HTTP_WRITE_DATA`，5/5 成功 |
| `cfaf436` | 请求只带最近 3 轮 user 历史（裁剪点落在 user 边界）；失败时重试一次 | 历史 7.1 KB→2.7 KB，但请求体仍 10 KB，工具轮次仍失败 |
| `e86838e` | 新增 `/api/netdiag`（DNS/TCP/HTTPS 三轮 + RSSI/堆）与 `/api/status` 链路字段 | 诊断手段，非修复 |

### TLS 接收缓冲实测矩阵（重要，别再踩）

| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | 结果 |
|---|---|
| 16 384（默认） | 握手能过，但写请求时 `alloc(4437) failed`，普通对话也不稳 |
| **8 192（当前）** | 普通对话稳定；工具轮次仍会 `alloc(5045) failed` |
| 6 144 | **握手直接失败**，`/api/netdiag` 0/3，证书链收不下 |
| 4 096 | 同上，全部 HTTPS 失败 |

**8 KB 是硬下限。** 不要再往下调。

### 其它已排除

- 不是 Wi-Fi：失败时 RSSI −37 dBm，`/api/netdiag` 的 DNS 6 ms、TCP 17 ms 正常
- 不是服务端：同一 key 从 PC 直接请求 1 秒返回；429 是另一个独立问题（账号 RPM 限制）
- 不是分区/Flash：固件 1.5 MB，factory 分区余量 46%
- 不是单实例并发：`/api/chat` 已有 busy 标记，测试时是串行请求
- 关掉配网热点能多出约 4 KB（71→75 KB），但**用户通过热点访问设备，不能关**

## 建议的解决方向（按性价比排序）

1. **复用 TLS 连接，跳过第二次握手**。工具轮次里两次请求打的是同一个 host，目前每次都 `esp_http_client_init()` + `cleanup()`，等于每次重新握手。若改成持久 client + `keep_alive_enable`，第二次请求省掉 8 KB 接收缓冲和 5 KB 证书校验，问题大概率直接消失。相关代码：`claw_llm_http_transport.c` 的 `claw_llm_http_post_json()`。
2. **静态 TLS 缓冲池**：在启动时（堆最干净时）预分配收发缓冲，避免峰值时找连续块。注意 C3 上会常驻占用约 10 KB。
3. **减少请求体基数**：人格 4.9 KB → 2.5 KB（需用户确认），工具清单裁掉未使用的系统类工具（约 1 KB）。预计省 3.4 KB。
4. **流式响应**：减少响应缓冲，但对手握阶段的峰值帮助有限。
5. **换带 PSRAM 的板子（ESP32-S3）**：彻底解决，但属于硬件变更，需用户决定。

## 复现步骤

```bash
# 1. 设备连上 Wi-Fi 后查 IP（串口启动日志里有 sta ip）
# 2. 普通对话应成功
curl -s -m 45 -X POST http://<IP>/api/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"只回复OK","session":"web-test"}'

# 3. 强制工具调用，观察是否在第二次请求失败
curl -s -m 60 -X POST http://<IP>/api/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"必须调用 web_search 搜索 ESP32-C3 的 GPIO 数量","session":"web-test"}'

# 4. 同时抓串口，过滤关键行
#    grep -aE "llm_http: POST|alloc.*failed|tool_call|tool_result|HTTP perform"
```

失败时注意 `llm_http: POST ... body=... free_heap=... largest=...` 这行日志，它记录了握手前的水位（`cfaf436` 新增）。

## 验收标准

- 连续 6 次带 `web_search` 的请求 **6/6 成功**
- 连续 5 次普通对话 **5/5 成功**
- 串口无 `Dynamic Impl: alloc(...) failed`、无 `Guru Meditation`
- 请求进行中网页仍可访问 `/api/status`

## 附：一次未复现的崩溃

压力测试中出现过一次 `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`，之后未能复现。若修复上述内存问题后仍出现，再单独排查。

## 相关代码位置

| 文件 | 作用 |
|---|---|
| `components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c` | LLM 的 HTTPS 请求、缓冲、重试 |
| `components/claw_capabilities/cap_web_search/src/cap_web_search.c` | 搜索工具、Tavily/Brave 请求与结果格式化 |
| `components/claw_modules/claw_memory/src/claw_memory_session.c` | 会话历史加载与请求裁剪 |
| `application/edge_agent/components/http_server/http_server_chat_api.c` | `/api/chat` 单实例闸门与超时 |
| `application/edge_agent/components/http_server/http_server_netdiag_api.c` | 网络自检接口 |
| `application/edge_agent/boards/community/esp32_C3_4MB/sdkconfig.defaults.board` | TLS / 内存相关 Kconfig |
