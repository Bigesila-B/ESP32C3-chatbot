# ChangeLog

本项目从 [espressif/esp-claw](https://github.com/espressif/esp-claw) 衍生，剥离为只服务 ESP32-C3 的独立固件。上游的变更记录不再维护，早期演进见 git 历史。

## 2026-11-17

### Change

* **项目独立化**：删除 S3 及其余板型、React 前端源码、文档站、Python 模拟器等上游产物，只保留 `esp32_C3_4MB` 一条硬件路线。仓库从 28 MB 降到 4.3 MB，删除 996 个文件。此后不再跟随 upstream 合并，`FORK_GUIDE.md` 一并移除，编译注意事项转到 [BUILD_NOTES.md](./BUILD_NOTES.md)。
* 默认烧录方式改为 `idf.py app-flash`，只更新 factory 分区，人格、记忆和凭证保留；`flash` 仅用于首次烧录或改分区表。
* 系统提示中的身份描述从 QQ 助手改为通用的 IM 助手，与多通道能力对齐。

### Feature

* **网页后台重写**：从单页配置表单改为七个分区——设备状态、对话、人格设定、长期记忆、对话模型、聊天通道、网络设置。纯手写 HTML + 原生 JS，hash 路由，无构建步骤，通过 `EMBED_FILES` 直接打进固件（约 26 KB）。
* **网页对话**：新增 `POST /api/chat`，同步调用 `claw_core_submit()` / `claw_core_receive()`，会话固定为 `web`，与 QQ 的历史互不干扰。回复只进 HTTP 响应，不推送到 IM。
* **人格编辑**：人格统一写在 `/memory/soul.md`，网页里单个 Markdown 文本框编辑。`app_claw_build_system_prompt()` 在每次请求前直接读该文件拼进系统提示，保存后下一次对话即生效，不需要重启。
* **长期记忆编辑**：`GET/POST/PUT/DELETE /api/memory` 对应条目的增删改查；新增 `POST /api/memory/bulk` 支持整块文本按行替换，返回 `kept/added/removed`；新增 `/api/memory/doc` 读写 `soul.md`、`identity.md`、`user.md`（白名单 + 8 KB 上限）。手动写入的条目 `source` 标记为 `manual`。
* **IM 通道扩展**：除 QQ 外，编译期同时打开 Telegram、飞书、微信三个通道，填好凭证重启即可启用。

### Fix

* `http_server` 的 `max_uri_handlers` 从 8 提升到 20（实际注册 14 条路由），修复新增记忆接口后启动即 `ESP_ERR_HTTPD_HANDLERS_FULL` abort 的问题。
* `TEXT_ONLY` 构建下排除 `cap_im_platform` 的 `cmd_cap_im_*.c`，这些文件依赖 `esp_console`，精简模式下不存在。
* `TEXT_ONLY` 构建下关闭飞书、Telegram、微信的附件入站，避免 768 KB 的 storage 被图片写满。
* `http_server` 组件补充 `claw_memory` 的 path 依赖声明，修复头文件搜索路径不通。
* `main/idf_component.yml` 显式声明 `esp_websocket_client`，避免被判为未使用而触发批量删除。
