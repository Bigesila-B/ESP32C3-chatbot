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
directly to `claw_core` with a per-request session id (default `web`).

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

Seven capability components are compiled: `cap_boards`, `cap_im_platform`,
`cap_llm_config`, `cap_session_mgr`, `cap_system`, `cap_web_search`,
`cap_memory_note`.
`cap_web_search` provides the `web_search` tool (Tavily or Brave) and is
registered only when at least one provider key is configured. `cap_memory_note`
provides `memory_remember` and is gated by the `memory_auto_record` setting.

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
provider injects in full.

Automatic recording is provided by `cap_memory_note`: a single
`memory_remember` tool that calls `claw_memory_store_with_result()` with
`source = "auto"`. It reuses the main agent's tool-call path, so no second LLM
runtime is started (the upstream async extraction worker requires
`MEMORY_MODE_FULL` and would not fit in the C3 heap). De-duplication,
timestamps and `MEMORY.md` regeneration all live in `claw_memory_store`.

The `memory_auto_record` NVS setting gates registration of that group. When it
is off, `app_claw_build_system_prompt()` appends an instruction telling the
model it cannot save memory — otherwise it would claim "I'll remember that"
without any way to persist it. Keep this coupling intact.

The `memory_remember` tool description, parameter hint, sample values and
result text are all in Chinese. This is deliberate: with an English description
the model wrote English memory entries even when told to match the user's
language. Do not translate that descriptor back to English without also
re-testing what language the stored entries end up in.

Three guards keep auto-recording from polluting memory: `cap_memory_note`
refuses credential-like content (`sk-`, `tvly-`, `密钥`, `密码`, `token`, ...);
`claw_memory_store_prepared_item_internal` applies a loose substring match
**only to `source=auto`** entries, so an appended restatement does not become
a second entry; and auto entries are capped at `CLAW_MEMORY_MAX_AUTO_ITEMS` (24)
because `MEMORY.md` is injected in full every request. Manual web-console
writes bypass the latter two.

Do NOT replace that substring test with character-overlap / edit-distance
similarity: Chinese short sentences that differ by one character (`猫`→`狗`,
`住北京`→`不住北京`) score near 1.0 and the user's fact update would be
silently dropped. Prefer an extra entry over losing a new fact — semantic
paraphrases with different wording can legitimately slip through here, since
there is no embedding model or spare LLM call on this chip.

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

- **Route count**: `http_server_core.c` sets `max_uri_handlers = 24` in the
  TEXT_ONLY build; 19 are in use. Every new endpoint must be counted;
  registration failure aborts at boot through `ESP_ERROR_CHECK`.
- **TLS receive buffer**: `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192` in the board
  defaults is a deliberate floor, not an oversight. 6 KB already fails the
  handshake outright (the certificate chain no longer fits); 16 KB leaves no
  room for the request peak. Do not "optimise" it downwards.
- **Request history is trimmed to the last 3 user turns** in
  `claw_memory_session_history_collect()`. The cut must stay on a `user`
  boundary: splitting an `assistant tool_calls` message from its `tool` results
  makes the upstream API reject the request. The full history stays on FATFS.
- **Do not copy the request body**: `claw_llm_http_transport.c` validates UTF-8
  in place and only allocates a sanitized copy when the body is actually
  invalid. The previous unconditional `calloc(len + 1)` doubled an 8 KB request
  at the exact moment mbedtls needed its TX record, which is what produced
  `ESP_ERR_HTTP_WRITE_DATA` on the device.
- **TLS send record**: `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048` keeps the
  per-record TX allocation around 2.4 KB instead of 4.4 KB. The body is split
  across more records; servers do not care, and the smaller block is far more
  likely to find a contiguous slot on a fragmented heap.
- **Single-flight chat**: `http_server_chat_api.c` holds a busy flag for the
  whole model call. `HTTP_CHAT_REPLY_MS` must stay above the configured LLM
  timeout (default 120 s); otherwise the handler returns, the flag clears while
  the LLM request is still running, and a second request stacks on top of it
  until the heap is gone.
- **Diagnostics endpoint**: `GET /api/netdiag` times DNS, TCP and a full HTTPS
  round trip three times and reports RSSI, channel, disconnect counters and
  free heap. `/api/status` carries the same link fields for cheap polling.
  Keep netdiag behind its heap guard (45 KB) — running it on a nearly full heap
  makes the situation worse, not better.
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
