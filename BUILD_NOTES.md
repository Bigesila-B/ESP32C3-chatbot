# 编译与烧录笔记

面向 ESP32-C3 单板型的实操记录。基础流程见 [README_CN.md](./README_CN.md)，本文补充环境细节、分区影响和踩过的坑。

## 环境

- ESP-IDF v5.5.x（本项目在 v5.5.6 上验证）
- 额外需要 `esp-bmgr-assist` 与 `idf-component-manager`：

```bash
pip install esp-bmgr-assist idf-component-manager
```

## 构建

工程根目录是 `application/edge_agent`，不是仓库根。

```bash
. $IDF_PATH/export.sh
cd application/edge_agent

# 只有首次检出、或改动了 boards/ 下的板型文件时才需要
idf.py bmgr -c ./boards -b esp32_C3_4MB

idf.py build
```

`idf.py bmgr` 会生成 `components/gen_bmgr_codes/`，里面包含板型的设备句柄和 Kconfig。这个目录是生成物，不入库，但**编译时必须存在**——缺了会在链接阶段报板型符号未定义。

生成的 `board_manager.defaults` 会自动进入 `SDKCONFIG_DEFAULTS` 链：

```
sdkconfig.defaults
  → components/gen_bmgr_codes/board_manager.defaults
  → build/edge_agent_partition_auto.defaults
```

所以板型的 Flash 大小和分区表由 `boards/community/esp32_C3_4MB/sdkconfig.defaults.board` 决定，改完要重跑 `idf.py bmgr` 再编译。

产物大小参考：`edge_agent.bin` 约 1.48 MB，factory 分区 2816 KB，剩余约 1.3 MB。

## 分区与烧录

`partitions_4MB.csv`：

| 分区 | 偏移 | 大小 | 内容 |
|---|---|---|---|
| `nvs` | 0xa000 | 24 KB | Wi-Fi、模型、IM 凭证等配置 |
| `phy_init` | 0x10000 | 4 KB | RF 校准 |
| `factory` | 0x20000 | 2816 KB | 应用固件 |
| `system` | 自动 | 384 KB | 只读 FATFS，内置人格与记忆模板 |
| `storage` | 自动 | 768 KB | 可写 FATFS，会话与长期记忆 |

两种烧录方式的差别是**会不会动数据分区**：

```bash
idf.py -p COM6 app-flash     # 只写 factory，人格、记忆、凭证全部保留
idf.py -p COM6 flash         # bootloader + 分区表 + factory + system + storage 全写
```

日常改代码用 `app-flash`。以下情况才需要 `flash`：首次烧录、改了分区表、改了 `fatfs_image/system` 里的默认模板。

`flash` 会覆盖 `storage.bin`，长期记忆、会话历史和配网信息会一起被重置，设备重新开热点等配网。

## 备份与回滚

4 MB Flash 全量备份 / 恢复：

```bash
python -m esptool --port COM6 --baud 460800 read-flash 0x0 0x400000 backup.bin
python -m esptool --port COM6 --baud 460800 write-flash 0x0 backup.bin
```

备份里含 Wi-Fi 密码和模型 API Key，不要上传到公开仓库。

## 已知陷阱

| 现象 | 原因 | 解法 |
|---|---|---|
| `std::filesystem::filesystem_error: Illegal byte sequence` | 源码路径含中文 | 换到纯 ASCII 路径编译 |
| `manifest file ... does not point to a directory` | 搬动过目录，`dependencies.lock` 里残留旧绝对路径 | 删掉 `dependencies.lock` 和 `managed_components/` 重新解析 |
| gcc/ccache 长时间停在同一个 obj，CPU 归零 | ccache 并发冲突 | `CMAKE_BUILD_PARALLEL_LEVEL=1` 单线程编译 |
| 组件重装被 `safe-delete` 拦截 | 单次删除文件数超过阈值 | 用 `Move-Item` 把目录挪到隔离区，让组件管理器重新下载 |
| 启动即 abort，日志里有 `ESP_ERR_HTTPD_HANDLERS_FULL` | `max_uri_handlers` 小于实际注册的路由数 | 改 `http_server_core.c` 里的槽位数，目前 TEXT_ONLY 分支为 20 |
| `esp_console.h: No such file` | `CONFIG_APP_CLAW_TEXT_ONLY=y` 时没有 console 组件 | `cap_im_platform/CMakeLists.txt` 里用 `if(NOT CONFIG_APP_CLAW_TEXT_ONLY)` 排除 `cmd_cap_im_*.c` |
| 组件被判 unused，触发批量删除 | 组件管理器认为依赖未使用 | 在 `main/idf_component.yml` 里显式声明该依赖 |
| `Cannot establish a connection to the component registry` | CMake 重新配置时组件管理器连不上 `components-file.espressif.com` | 设 `IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn` 走国内镜像 |
| ninja 长时间停在同一 obj，gcc/ccache 的 CPU 归零 | 本机 ccache 并发死锁 | `CCACHE_DISABLE=1` + `CMAKE_BUILD_PARALLEL_LEVEL=2`，杀干净残留进程后重跑 |
| 工具调用时 `Out of memory serializing request` | 第二轮请求要带人格、历史与工具结果，C3 堆不够 | 控制工具输出长度与人格文件大小；`CLAW_CAP_CORE_OUTPUT_SIZE` 在本项目已降到 8 KB |

`idf.py build` 不支持 `-j N`，限制并发要用 `CMAKE_BUILD_PARALLEL_LEVEL` 环境变量。

后台忘记管理密码时，只能擦除 NVS 分区恢复出厂（同时清掉 Wi-Fi 与所有密钥）：

```bash
python -m esptool --port COM5 erase_region 0xa000 0x6000
```

新加 `/api/*` 路由时记得两件事：用 `HTTP_SERVER_GATED_HANDLER` 包一层认证，以及核对 `http_server_core.c` 里的 `max_uri_handlers`（当前 24 槽、用了 19 条）。

## 在别处建构建目录

如果源码放在带中文的路径、需要另建一个纯 ASCII 的编译目录，注意别把生成物一起镜像掉：

```powershell
robocopy <源码> <构建目录> /MIR /XD .git build managed_components gen_bmgr_codes `
    /XF sdkconfig sdkconfig.old dependencies.lock
```

漏掉这些排除项会把 `sdkconfig`、`gen_bmgr_codes/`、`managed_components/` 删掉——它们都在 `.gitignore` 里，源码侧没有对应文件。补救办法是重跑 `idf.py bmgr` 再编译，配置会从默认值重新生成。
