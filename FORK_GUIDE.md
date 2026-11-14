# 开发方向与维护指南

本文说明这个 fork 的**定位、分支策略、当前定制功能、以及如何同步官方更新**。
如果你是一个 agent 或协作者，读完这篇就能理解项目意图，不必先读源码。

---

## 1. 项目定位

这是 [espressif/esp-claw](https://github.com/espressif/esp-claw) 的个人优化分支。

**核心目标：**

> 在官方固件基础上叠加**局部、低耦合**的优化，同时保持**能低成本跟进官方新版**。

**这意味着三条设计约束：**

1. **上游代码保持原样。** 不重构、不重命名、不搬目录。官方文件的结构和命名一律不动。
2. **定制改动尽量收敛。** 优先"新增独立函数 + 在原函数里插一个调用"，而不是把原函数推倒重写。
3. **每个定制功能都要能独立讲清楚。** 知道它改了哪些文件、锚点在哪、怎么验证，迁移时才能快速定位冲突。

**反面做法（不要做）：**

- 直接改官方组件的对外接口，导致上游一改就大面积冲突
- 把多个不相关的优化混在一个提交里
- 在 `master` 分支上改代码（会失去快进合并的能力）

---

## 2. 分支模型

```
upstream  espressif/esp-claw
              │
              │  fetch + merge（快进）
              ▼
master  ──────────────────────────►  始终等于官方最新，不加任何本地提交
              │
              │  rebase（迁移定制功能）
              ▼
feature/context-compaction  ──────►  定制功能分支，设备实际烧录的版本
```

| 分支 | 作用 | 规则 |
|---|---|---|
| `master` | 跟踪官方最新 | **只允许快进合并，禁止直接提交** |
| `feature/context-compaction` | 当前定制功能集合 | 基于上游某个基线，rebase 到新 master 来升级 |

`master` 一旦混入本地提交，就无法快进合并官方更新，整个模型就崩了。**这是最重要的一条纪律。**

---

## 3. 当前定制功能

基线：上游 commit `a4edfe5`（2026-07-28）。

### 3.1 按上下文窗口触发压缩 + LLM 摘要

**解决的问题**：原版只在会话文件超过 150KB 时压缩，且压缩时无条件保留全部
user/assistant 文本，只丢工具记录。纯文字聊天永远压不下去，到 150KB 后写
`.blocked` 标记，该会话被永久拒绝，只能 `/session new`。

**改动清单：**

| 文件 | 改动性质 | 关键锚点 |
|---|---|---|
| `components/claw_modules/claw_memory/src/claw_memory_session.c` | 新增 ~330 行 + 改 3 个函数 | 见下方 |
| `components/claw_modules/claw_memory/src/claw_memory_internal.h` | 加 6 个宏、state 加 2 字段 | `CLAW_MEMORY_SESSION_SIZE_LIMIT` 附近 |
| `components/claw_modules/claw_memory/include/claw_memory.h` | `claw_memory_config_t` 加 1 字段 | `max_tool_iterations` 之后 |
| `components/claw_modules/claw_memory/src/claw_memory.c` | `claw_memory_init` 加 2 行 | `s_memory.backend_format = ...` 之后 |
| `components/common/app_claw/include/app_claw.h` | `app_claw_config_t` 加 1 字段 | `llm_max_tokens` 之后 |
| `components/common/app_claw/app_claw.c` | `init_memory` 加 1 行 | `.max_tool_iterations` 之后 |
| `application/edge_agent/components/app_config/include/app_config.h` | `app_config_t` 加 1 字段 | `llm_max_tokens` 之后 |
| `application/edge_agent/components/app_config/app_config.c` | 默认值宏 + 字段表 + to_claw | 3 处，均在 `llm_max_tokens` 附近 |
| `application/edge_agent/components/http_server/http_server_config_api.c` | `CONFIG_FIELDS` 加 1 行 | `llm_max_tokens` 那一行之后 |

**`claw_memory_session.c` 内部结构（迁移时按这个顺序对照）：**

1. `session_history_analyze_turns()` 之后、`session_history_compact_keep_record()` 之前，
   插入一整块新函数：
   - `s_compact_keep_recent`（文件级静态变量，控制保留轮数）
   - `session_history_context_window_for_model()`（模型名 → 窗口）
   - `session_history_estimate_tokens()`（字节 → token 估算）
   - `session_history_summary_path_dup()` / `_for_session()`
   - `session_history_save_summary()`
   - `session_history_generate_summary()`（调 LLM 出摘要）
2. `session_history_compact_keep_record()`：把 `USER/ASSISTANT_FINAL` 的
   无条件 `return true` 改成 `return in_recent_window`
3. `session_history_compact_if_needed()`：触发条件从纯字节改成
   "字节兜底 + token 软阈值"，并加入保留轮数收缩、摘要生成两个步骤
4. `claw_memory_session_history_collect()`：在返回历史 JSON 前，
   把摘要插成数组前两条消息

**验证：**

```bash
# 编译产物里有这些符号
xtensa-esp32s3-elf-nm build/edge_agent.elf | grep -E "session_history_generate_summary|s_compact_keep_recent"

# 运行日志（触发压缩时出现）
# I (xxxx) claw_memory: session compact trigger session=cli bytes=... est_tokens=... window=... model=...
# I (xxxx) claw_memory: session summary saved session=cli chars=...
```

### 3.2 WebUI 可配置上下文窗口

**解决的问题**：窗口大小硬编码在自动匹配表里，用户无法覆盖。

**改动清单：**

| 文件 | 改动 |
|---|---|
| `frontend_source/src/api/client.ts` | `AppConfig` 类型加 `llm_context_window`；`GROUP_FIELDS.llm` 加同名项 |
| `frontend_source/src/i18n/zh-cn.ts` | 加 `llmContextWindow*` 3 个 key |
| `frontend_source/src/i18n/en.ts` | 同上（英文） |
| `frontend_source/src/pages/LlmPage.tsx` | 在「最大 Token 数」输入框后加一个 `TextInput` |
| `frontend_source/pnpm-workspace.yaml` | 加 `packages: []`（pnpm 9 必需） |
| `frontend_source/dist/index.html.gz` | 重新构建的产物 |

配置链路：WebUI → `http_server_config_api.c` 的 `CONFIG_FIELDS` → `app_config_t`
→ NVS key `llm_ctx_window` → `app_claw_config_t` → `claw_memory_config_t`
→ `s_memory.context_window_tokens` → 压缩触发时读取。

**注意**：前端改动**必须重新构建**，否则 `dist/index.html.gz` 是旧的，
WebUI 上看不到新输入框。

```bash
cd application/edge_agent/components/http_server/frontend_source
pnpm install
pnpm run build
```

### 3.3 `/new` 命令别名

**解决的问题**：官方 FAQ 让用户发 `/new`，但解析器只认裸子命令 `new`，
实际必须发 `/session new`。

| 文件 | 改动 |
|---|---|
| `components/claw_capabilities/cap_session_mgr/src/cap_session_mgr.c` | `cap_session_mgr_session_execute()` 入口加前缀归一化 |

归一化规则：`/session new` → `new`，`/new` → `new`，`/list` → `list`，裸词保持不变。

---

### 3.4 聊天指令列表和 token 用量

主实现位于 `application/edge_agent/components/app_chat_help/`，前端页面为 `ChatHelpPage.tsx`。Web 的 `/api/chat/help` 和聊天中的 `/help` 读取同一份指令来源。设备读取启用的斜杠路由，借用现有 `session_command` 和 `llm_config_command` 的帮助输出列出子指令。

同步新版时可以从 `upstream/master` 建一个本地验证分支，先迁移依赖的定制提交，再 `git cherry-pick` 聊天指令提交。按新版接口对照下表接入。

| 上游入口 | 接入内容 |
|---|---|
| `claw_event_router` | 注册一个可消费聊天消息的回调，回复沿用已有 IM 出站绑定。无需给用户已有的路由 JSON 添加规则。 |
| `claw_llm_http_post_json()` | 在一次成功的 LLM 响应交给后端解析前，将响应体交给用量统计。这个入口覆盖对话、工具循环、视觉和记忆的模型请求。 |
| `edge_agent/main.c` | 设置时区并初始化 NVS 后启动统计，Agent 启动后注册聊天回调。 |
| `http_server` 和前端导航 | 挂载 `/api/chat/help` 和指令列表页。旧 Web IM 从 WebSocket 消息的 `kind` 渲染回复图标。新版 `http_server_webim_send_session()` 直接提交 Agent 请求，需在该入口提交前处理 `/help`，再由会话聊天页面渲染帮助回复和 favicon。 |
| `cap_session_mgr` 和 `cap_llm_config` | 用各自已有的用法说明维护子指令和别名。单独运行 `/session` 或 `/llm` 时也会显示这些说明。 |

`app_chat_help.c` 中的描述表只补充中英文文案。新增内置指令时按命令名维护描述，指令是否显示仍取决于设备实际启用的路由。自定义路由沿用自己的描述。

用量用设备时区分日、分月累计，只计模型响应中的真实用量。NVS 命名空间为 `claw_usage`，键为 `totals_v1`。换基线时保留此记录格式，或迁移旧值。生成前端资源后重新构建固件。回归时检查网页指令和 `/help` 一致、`/help` 不触发模型请求、普通对话增加计数、重启后统计可读。

以官方 `50072775` 为基线做过三方合并检查。独立组件、事件路由和模型 HTTP 入口可自动合并。Web 会话接入、菜单和文案、主程序初始化、模型命令帮助需要按新版接口合并。`dist/index.html.gz` 随新版源码重新生成。

### 3.5 网页图片上传

`frontend_source/src/utils/imageUpload.ts` 在浏览器中处理超过 512 KiB 的图片。在线聊天和文件管理都在调用 `uploadFile()` 前准备文件。压缩后的文件使用 `.jpg` 扩展名。移植时接到新版的两个文件选择入口，并与 `HTTP_SERVER_UPLOAD_MAX_SIZE` 对齐。用超限图片、原尺寸小图和无法解码的图片分别验证上传结果。

### 3.6 可写分区耗尽时的处理

会话历史无可读记录且索引重建失败时，`claw_memory_session_load_json_alloc()` 记录错误并允许本轮聊天继续。Skill 和 Web 上传检查 `fclose()` 的结果，避免缓冲写入失败后误报成功。空的 Skill 会话记录可在有可用空间后重新激活。迁移时保留新版的索引修复流程和 Skill 注册接口，复测磁盘满时的文本回复及恢复后的 Skill 激活。

## 4. 同步官方更新

### 一次性配置

```bash
git remote add upstream https://github.com/espressif/esp-claw.git
```

### 在独立工作区验证新版

```bash
git fetch upstream
git worktree add -b upgrade/esp-claw ../esp-claw-upgrade upstream/master
cd ../esp-claw-upgrade
git log --oneline --reverse upstream/master..local/chat-help
```

用 `git cherry-pick <提交>` 按顺序迁移仍需要的本地功能。根据第 3 节核对新版入口，重新构建前端和固件，再用测试设备验证。原有工作区和分支仍可用于回退。

---

## 5. 迁移冲突处理

迁移上下文压缩时，优先对照新版 **`claw_memory_session.c`** 的记录格式和压缩逻辑。

**处理顺序：**

1. **先保留上游版本，再整块插入新增函数。**
   在 `cherry-pick` 发生冲突时，可以用 `git show HEAD:<文件路径>` 查看目标分支版本。
   若新版仍保留相同的会话存储接口，可把第 3.1 节的新函数接到 `session_history_analyze_turns()` 之后。

2. **逐个改 3 个函数，不要重写。**
   - `session_history_compact_keep_record()`：只改 `return true` 那一行为窗口判断
   - `session_history_compact_if_needed()`：保留上游的校验与重置逻辑，只替换
     触发判断和追加摘要步骤
   - `claw_memory_session_history_collect()`：只在返回前插入摘要注入

3. **核对相关配置字段。**
   上游字段顺序变化时按字段名对齐。

**判断上游是否重构了相关逻辑：**

```bash
git log -20 upstream/master --oneline -- components/claw_modules/claw_memory/src/claw_memory_session.c
```

如果上游已经把压缩改成基于 token 或加了摘要功能，那么 3.1 的定制就可以**整体废弃**，
直接采用官方实现——这也是保留 fork 结构的价值之一。

---

## 6. 编译与烧录

### 硬件

- ESP32-S3 N16R8（16MB Flash + 8MB PSRAM）
- 板型配置：`esp32_S3_DevKitC_1_breadboard`
- 串口：板载 CH343（`VID_1A86:PID_55D3`）

### 环境

```bash
# ESP-IDF v5.5.x
git clone --depth 1 -b release/v5.5 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3            # Windows: install.bat esp32s3
. ./export.sh
pip install esp-bmgr-assist idf-component-manager
```

### 构建

```bash
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1_breadboard
idf.py build
idf.py -p COM4 flash
```

### 已知编译陷阱

| 现象 | 原因 | 解法 |
|---|---|---|
| `std::filesystem::filesystem_error: Illegal byte sequence` | 源码路径含中文 | 移到纯 ASCII 路径 |
| `manifest file ... does not point to a directory` | 移动目录后 `dependencies.lock` 里残留旧的绝对路径 | 批量替换 lock 里的 `source.path` |
| gcc/ccache CPU 归零、进度长时间停在同一 obj | ccache 并发冲突（多个进程共用 `cpp_stdout.tmp.*.i`） | `CMAKE_BUILD_PARALLEL_LEVEL=1` 单线程编译 |
| 组件重装被 `safe-delete` 拦截 | 单次删除超过阈值 | 用 `Move-Item` 隔离目录，让管理器重新下载 |
| `packages field missing or empty` | pnpm 9 要求 workspace 文件有 `packages` | 加 `packages: []` |

---

## 7. 刷机注意事项

烧录 `edge_agent.bin`（`0x20000`）不影响 NVS（`0xA000`），Wi-Fi 和 API Key 会保留。

烧录 `system.bin` / `storage.bin` 会**重置文件系统**，会话历史和上传的文件会丢。

完整备份 / 回滚：

```bash
python -m esptool --port COM4 --baud 460800 read-flash 0x0 0x1000000 backup.bin
python -m esptool --port COM4 --baud 460800 write-flash 0x0 backup.bin
```

---

## 8. 后续可做的方向

按价值和成本排序，供参考：

1. **压缩触发比例可配**：目前 `CTX_TRIGGER_PERCENT` 是编译期常量，可以像窗口一样开放到 WebUI
2. **保留轮数可配**：同上，`SUMMARY_KEEP_RECENT`
3. **摘要回看**：把摘要文件内容暴露成一个 `/summary` 命令，方便确认压缩没丢关键信息
4. **压缩统计**：记录压缩次数、累计丢弃字节，便于评估效果
5. **自动清理旧会话**：`/session list` 加使用率统计，接近分区上限时提示

做新功能时请沿用同一套纪律：新文件、新函数优先，改动点写进本文档第 3 节。
