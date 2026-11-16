# ESP32C3-chatbot

一块 ESP32-C3（4 MB Flash）上的 QQ 聊天机器人，自带网页管理后台。对话、记忆和配置全部跑在板子上，不需要额外的服务器。

## 功能

**聊天**

- 接入 QQ 开放平台，群聊 @ 和单聊都能用
- 同一份固件也编译了飞书、Telegram、微信通道，填好凭证重启即可启用
- 网页后台里可以直接和板子对话，使用独立的 `web` 会话，与 QQ 的历史互不干扰

**记忆**

- 会话历史持久化在设备上，重启可以续聊
- 长期记忆按条目存储，网页上每行一条，可增删改
- 上下文接近模型窗口时按 token 阈值触发压缩，较早的内容生成摘要后保留

**人格**

- 人格写在 `/memory/soul.md`，网页里直接编辑，保存后下一次对话就生效
- 固件在拼装系统提示时读取该文件，不需要重启

**网页后台**

七个分区：设备状态、对话、人格设定、长期记忆、对话模型、聊天通道、网络设置。纯手写 HTML 加原生 JS，没有构建步骤，直接打进固件。

## 硬件

- ESP32-C3，4 MB Flash
- 板型配置 `esp32_C3_4MB`
- 分区布局：factory 2816 KB / system 384 KB / storage 768 KB

当前固件约 1.5 MB，占 factory 分区的 52%，剩余约 1.3 MB。

需要注意的约束是 RAM 而不是 Flash：C3 没有 PSRAM，启动时可用堆约 262 KB，Wi-Fi 与 TLS 之后实际可用的通常在 60–100 KB。增加常驻内存的功能前先确认余量。

## 编译

需要 ESP-IDF v5.5.x。

```bash
. $IDF_PATH/export.sh
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_C3_4MB
idf.py build
idf.py -p COM6 flash
```

首次烧录后设备会开一个配网热点 `esp-claw-XXXXXX`。连上热点，浏览器打开 <http://192.168.4.1>，填写 Wi-Fi、模型接口和 QQ 机器人参数，保存后重启。

环境细节、分区布局和踩过的坑见 [BUILD_NOTES.md](./BUILD_NOTES.md)。

## 升级

改完代码只更新应用分区：

```bash
idf.py -p COM6 app-flash
```

人格、长期记忆和 Wi-Fi 凭证都会保留。

`idf.py flash` 会连同 storage 分区一起覆盖，人格和记忆会被重置。只在首次烧录或修改分区表时使用。

## 目录

```
application/edge_agent/            主应用
  boards/community/esp32_C3_4MB/   唯一的板型定义
  components/                      应用层组件：配置、HTTP 服务、聊天帮助
  fatfs_image/                     内置只读文件系统，含人格与记忆的默认模板
  partitions_4MB.csv               4 MB 分区表
components/
  claw_capabilities/               能力组件：板型、IM 通道、模型配置、会话、系统
  claw_modules/                    Agent 内核：core、memory、event router、cap、skill
  common/                          应用装配、Wi-Fi 管理、设置存储、HTTP 复用
```

## 许可证

Apache-2.0，见 [LICENSE](./LICENSE)。

设备端的 agent 框架源自 [Espressif ESP-Claw](https://github.com/espressif/esp-claw)。本项目在其基础上做了裁剪和重写，只服务 ESP32-C3 这一种硬件，不再跟随上游更新。上游的版权声明保留在各源文件中。
