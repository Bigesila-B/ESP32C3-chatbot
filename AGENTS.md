# Agents.md

Guidance for agents working on this repository.

## Project Overview

A QQ chatbot for a single ESP32-C3 board with 4 MB of flash. The firmware is an
ESP-IDF application that runs an agent loop on the device: it receives messages
from IM channels, calls an OpenAI-compatible model, executes tool calls, keeps
session history and long-term memory on flash, and serves a web console.

This is a standalone project. It started from Espressif ESP-Claw but no longer
tracks upstream, and code that C3 does not use has been removed. Do not assume
directories that exist upstream are present here.

## Build

ESP-IDF v5.5.x is required.

```bash
. $IDF_PATH/export.sh
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_C3_4MB
idf.py build
```

`idf.py bmgr` must run before the first build and after any change to the board
folder or a fresh checkout — it generates `components/gen_bmgr_codes`.

To update a running board without wiping data, use `idf.py -p PORT app-flash`.
A full `flash` also overwrites the storage partition and resets persona and
memory.

## Architecture

### Boot flow

`application/edge_agent/main/main.c` initialises storage, board peripherals,
Wi-Fi, the configuration store, capabilities and the HTTP server, then starts
the agent runtime.

### Data flow

1. An IM channel (QQ, Feishu, Telegram, WeChat) or the web console receives a
   message.
2. `claw_event_router` matches the event against `router_rules.json` under the
   DATA root and decides what to run.
3. `claw_core` builds context from memory, session history and other providers,
   calls the model, executes capability tool calls and persists the result.
4. Replies go back through the originating channel.

The web console's chat panel bypasses the router: `POST /api/chat` submits
directly to `claw_core` with the fixed session id `web`.

### Key components

- `components/claw_modules/claw_core` — request queue, context building, LLM
  runtime, tool-call loop, context persistence.
- `components/claw_modules/claw_memory` — session history, persona files,
  long-term memory entries, compaction.
- `components/claw_modules/claw_event_router` — declarative routing.
- `components/claw_capabilities/cap_im_platform` — all IM channels in one
  component, one source file per platform.
- `application/edge_agent/components/http_server` — web console, REST API and
  the embedded settings page.
- `application/edge_agent/components/app_config` — NVS-backed settings with a
  single field table driving load, save and JSON serialisation.

### Enabled capabilities

Six capability components are compiled: `cap_boards`, `cap_im_platform`,
`cap_llm_config`, `cap_session_mgr`, `cap_system`, `cap_web_search`.
`cap_web_search` provides the `web_search` tool (Tavily or Brave) and is
registered only when at least one provider key is configured.

Lua, MCP, MicroPython, skills, scheduling and file browsing remain removed.
Do not add them back without re-evaluating RAM: the build is
`CONFIG_APP_CLAW_TEXT_ONLY=y`, the image uses 52% of the factory partition,
and the C3 has no PSRAM. Tool calls are memory-sensitive here:
`claw_cap` allocates a `CLAW_CAP_CORE_OUTPUT_SIZE` buffer per call (8 KB in
this project), the persona alone is ~6 KB of context, and a second LLM turn
carries the tool result. Keep tool output short.

`llm_supports_tools` must be `"true"` for any tool to reach the model. The
tools context provider and the `supports_tools` flag were originally gated out
of `TEXT_ONLY` builds in upstream; both gates were removed here.

`restart_device` in `cap_system` has `cap_flags = 0` so the model cannot reboot
the board. Device restart stays on the manual `/api/restart` path.

## Runtime paths

- `CLAW_PATH_SYSTEM` (`/system`) is read-only: firmware-baked defaults,
  including `.recovery/memory/soul.md` used to seed persona files.
- `CLAW_PATH_DATA` (`/fatfs`) is writable: `memory/soul.md`, `memory/user.md`,
  `memory/identity.md`, `MEMORY.md`, session files and router rules.
- Use `claw_paths_join(CLAW_PATH_DATA, ...)`; never hard-code `/fatfs`.

The persona and profile files are injected by
`claw_memory_profile_provider` on every request, reading `soul.md`, `user.md`
and `identity.md` fresh from the DATA root. `app_claw_build_system_prompt()`
only returns the short fixed prefix — do not add a second file read there:
injecting the persona twice pushed the C3 past its heap limit once tools were
enabled.

Long-term memory entries are rendered into `MEMORY.md`, which the lightweight
provider injects in full. There is no automatic memory extraction in
`MEMORY_MODE_LIGHTWEIGHT`; entries are written only by the web console API.

## Web console

`application/edge_agent/components/http_server/frontend_text/index.html` is the
entire UI: one file, no build step, embedded with `EMBED_FILES`. Editing it only
requires a rebuild, not a bundler. Keep it lean (currently ~33 KB) and avoid
features that break older mobile browsers; it targets phones on the
provisioning hotspot.

The chat page has a session sidebar backed by `/api/sessions`, `/api/session`
and `DELETE /api/session`. `POST /api/chat` takes an optional `session` field
(alphanumeric, `_`, `-`, max 32 chars); the default is `web`. Session files are
`s_<id>_<hash>.log` under `/fatfs/sessions` — the HTTP layer reverse-parses
the id from the filename instead of reimplementing the hash.

## Common pitfalls

- **Route count**: `http_server_core.c` sets `max_uri_handlers = 20` in the
  TEXT_ONLY build; 17 are in use. Every new endpoint must be counted;
  registration failure aborts at boot through `ESP_ERROR_CHECK`.
- **Component dependencies**: a path dependency listed in `idf_component.yml`
  without `rules` is resolved unconditionally. Deleting such a component breaks
  the CMake configure step even when its Kconfig option is off.
- **Component registry**: `components-file.espressif.com` may be unreachable in
  mainland China. When CMake re-runs and fails with "Cannot establish a
  connection to the component registry", set
  `IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn`.
- **ccache deadlock**: on this machine concurrent ccache processes can hang with
  near-zero CPU and stall ninja indefinitely. Build with `CCACHE_DISABLE=1` and
  `CMAKE_BUILD_PARALLEL_LEVEL=2` when that happens.
- **Heap headroom**: `Out of memory serializing request` means the second LLM
  turn (with tool result) could not be serialized. Keep tool output, persona and
  session history small; do not reintroduce duplicate context providers.
- **Build cache**: after removing components, delete the `build` directory.
  `idf.py fullclean` refuses when `CMakeCache.txt` is missing; use
  `[System.IO.Directory]::Delete("\\?\path", $true)` on Windows for long paths.
- **Non-ASCII paths**: building from a path containing Chinese characters fails
  inside CMake. Copy the tree to an ASCII path first.
- **Generated board files**: `components/gen_bmgr_codes` is generated, never
  edit it by hand.

## Code style

- C, not C++, following ESP-IDF object conventions.
- Each module is an opaque handle: `typedef struct xxx_t *xxx_handle_t`, with
  the struct defined only in the `.c` file.
- Public APIs return `esp_err_t`, take `xxx_handle_t` first, and follow
  `create/delete/start/stop/get/set` naming.
- Keep source files under about 1500 lines and split by responsibility.
- Avoid magic numbers; use named constants, enums or Kconfig options.
- Check return values, handle allocation failures, release partial resources.
- Protect shared mutable state with a mutex and document ownership.
- Avoid stack buffers larger than 128 bytes; use heap or file-scope buffers.

## Testing

- Firmware changes: `idf.py bmgr ... && idf.py build` must pass for
  `esp32_C3_4MB`. There is no CI.
- Flash with `app-flash` and check the serial log for panics; `ESP_ERROR_CHECK`
  failures show up as a reboot loop.
- Web console changes: load the page from the device IP and confirm each section
  renders; the API contract is in `http_server_*_api.c`.
