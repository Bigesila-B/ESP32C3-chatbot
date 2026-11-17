# ChangeLog

本项目从 [espressif/esp-claw](https://github.com/espressif/esp-claw) 衍生，剥离为只服务 ESP32-C3 的独立固件。上游的变更记录不再维护，早期演进见 git 历史。

## 2026-11-17

### Change

* **项目独立化**：删除 S3 及其余板型、React 前端源码、文档站、Python 模拟器等上游产物，只保留 `esp32_C3_4MB` 一条硬件路线。仓库从 28 MB 降到 4.3 MB，删除 996 个文件。此后不再跟随 upstream 合并，`FORK_GUIDE.md` 一并移除，编译注意事项转到 [BUILD_NOTES.md](./BUILD_NOTES.md)。
* 默认烧录方式改为 `idf.py app-flash`，只更新 factory 分区，人格、记忆和凭证保留；`flash` 仅用于首次烧录或改分区表。
* 系统提示中的身份描述从 QQ 助手改为通用的 IM 助手，与多通道能力对齐。

### Feature

* **网页后台重写**：从单页配置表单改为七个分区——设备状态、对话、人格设定、长期记忆、对话模型、聊天通道、网络设置。纯手写 HTML + 原生 JS，hash 路由，无构建步骤，通过 `EMBED_FILES` 直接打进固件（初版约 26 KB，现约 33 KB）。
* **网页对话**：新增 `POST /api/chat`，同步调用 `claw_core_submit()` / `claw_core_receive()`，默认会话 id 为 `web`，也可通过 `session` 参数指定独立会话，与 QQ 的历史互不干扰。回复只进 HTTP 响应，不推送到 IM。
* **人格编辑**：人格统一写在 `/memory/soul.md`，网页里单个 Markdown 文本框编辑。`app_claw_build_system_prompt()` 在每次请求前直接读该文件拼进系统提示，保存后下一次对话即生效，不需要重启。
* **长期记忆编辑**：`GET/POST/PUT/DELETE /api/memory` 对应条目的增删改查；新增 `POST /api/memory/bulk` 支持整块文本按行替换，返回 `kept/added/removed`；新增 `/api/memory/doc` 读写 `soul.md`、`identity.md`、`user.md`（白名单 + 8 KB 上限）。手动写入的条目 `source` 标记为 `manual`。
* **IM 通道扩展**：除 QQ 外，编译期同时打开 Telegram、飞书、微信三个通道，填好凭证重启即可启用。
* **联网搜索**：移植 `cap_web_search`（Tavily 优先、Brave 备选），模型通过 `web_search` 工具自主发起查询；后台可填两家 API Key，未配置密钥时不注册该工具。为适配 C3 的堆，结果数降到 3 条、正文按 UTF-8 边界截成短摘要。
* **聊天记录侧边栏**：对话页左侧列出设备上的历史会话，支持新建、切换、删除；新增 `GET /api/sessions`、`GET/DELETE /api/session`，`/api/chat` 增加可选 `session` 参数（默认 `web`）。会话 id 由文件名反解，不重复实现哈希。
* **工具调用开关**：模型设置页新增「工具调用」下拉并默认开启；搜索能力依赖它。
* **长期记忆自动记录**：新增 `cap_memory_note` 组件，暴露 `memory_remember` 工具，让模型把对话中的持久事实写进长期记忆，`source=auto`；复用主 agent 的工具调用链路，不额外启动 LLM runtime（上游 FULL 模式的异步抽取 worker 在 C3 上放不下）。去重、时间戳、`MEMORY.md` 渲染全部沿用 `claw_memory_store_with_result()`。
* 长期记忆页新增「自动记录」开关（`memory_auto_record`，默认开启）与自动条目统计；关闭时工具不注册，并向系统提示追加一句约束，避免模型口头承诺"记住了"却无法落盘。

### Fix

* `http_server` 的 `max_uri_handlers` 从 8 提升到 20（当时注册 14 条路由，加入会话接口后为 17 条），修复新增记忆接口后启动即 `ESP_ERR_HTTPD_HANDLERS_FULL` abort 的问题。
* `TEXT_ONLY` 构建下排除 `cap_im_platform` 的 `cmd_cap_im_*.c`，这些文件依赖 `esp_console`，精简模式下不存在。
* `TEXT_ONLY` 构建下关闭飞书、Telegram、微信的附件入站，避免 768 KB 的 storage 被图片写满。
* `http_server` 组件补充 `claw_memory` 的 path 依赖声明，修复头文件搜索路径不通。
* `main/idf_component.yml` 显式声明 `esp_websocket_client`，避免被判为未使用而触发批量删除。
* 上游在 `TEXT_ONLY` 构建里同时排除了「工具清单 provider」和 `supports_tools` 声明，导致模型拿不到任何工具；两处限制已解除，联网搜索才真正可用。
* 撤销上一轮的人格直读补丁：`claw_memory_profile_provider` 原本就会注入 `soul.md`，重复注入使每个请求多背 1.8 KB，开启工具后直接 OOM。
* `claw_cap` 每次工具调用申请的 32 KB 输出缓冲降为 8 KB；`restart_device` 改为模型不可调用，重启只保留网页 `/api/restart` 这条人工通道。
* `http_server` 的 `max_uri_handlers` 从 20 保持不变（会话接口占 3 个槽位，现共 17 条路由）。
* 自动记忆条目的工具描述、参数说明与返回文本统一改为中文并给出中文示例，模型写入的条目随之变为中文（此前英文描述里夹一句语言要求无效，模型仍输出英文）。
* **自动记忆防乱记**：实测后补三道护栏 —— 工具层拦截密钥/密码/令牌等凭证（条目会明文落 flash 并随每次请求外发）；底层仅对 `source=auto` 放宽去重（只认「一条是另一条的连续子串且长度接近」，不用相似度，避免把「猫→狗」「住北京→不住北京」这类事实更新误判为重复而丢弃）；自动条目上限 24 条，防止 `MEMORY.md` 膨胀把 C3 堆挤爆。网页手工写入不受后两项约束。

### 子 Agent 审查后修复

* **纠正一处会丢数据的去重逻辑**：审查实测发现上一版的字节二元组重合率会把「用户养了一只猫 / 狗」（0.90）、「用户住在北京 / 不住在北京」（1.00）判成同一条，导致事实更新被静默丢弃。改为「连续子串 + 长度接近」的保守判定，宁可多存一条也不丢新事实。
* **UTF-8 安全**：搜索摘要补全字节校验，会话标题与回放改为按码点边界截断，修掉半个字符进 JSON 的问题；搜索响应缓冲加 16 KB 硬上限并标记溢出，防止无限 `realloc` 打爆堆。
* **编译期一致性**：`app_claw_bool_is_true()` 移出 `CAP_MEMORY` 条件块（它被 `CAP_CORE` 路径共用）；会话 id 从文件名反解后再过一次白名单校验。
* **前端**：`chatBusy` 期间禁止新建/切换/删除会话，并快照会话 id 防止旧回复串到新会话；`savePage` 改用 `PAGE_FIELDS` 白名单并校验数值字段；移动端补 `min-width:0` 防横向溢出。
* `.gitignore` 补 `**/gen_bmgr_codes/` 与 `board_manager.defaults`，与 `BUILD_NOTES.md` 的描述一致。
* 文档自查：修正「会话固定为 web」「14 条路由」「页面约 26 KB」「能力组件 5 个」等与新实现矛盾的表述，英文 README 补齐护栏说明。
