# ESP32C3-chatbot

A QQ chatbot running on a single ESP32-C3 with 4 MB of flash, with a built-in web console. Chat, memory and settings all live on the board — no extra server needed.

## Features

**Chat**

- Connects to the QQ open platform; works in groups (via @) and direct messages
- The same firmware also builds the Feishu, Telegram and WeChat channels — fill in credentials and reboot to enable them
- The web console has its own chat panel with a session sidebar: create, switch and delete conversations stored on the board
- Tavily or Brave web search is available; the model calls `web_search` when a question needs up-to-date information

**Memory**

- Session history persists on the device, so conversations survive reboots
- Long-term memory is stored as entries; the web console edits them one per line
- With **Auto record** enabled, the model writes durable facts (name, project, hardware, stable preferences) into memory on its own, tagged `auto`; the device de-duplicates them
- Auto record reuses the tool-call path, so it needs no extra model runtime or request. Turning it off removes the write tool from the model
- When the context approaches the model window, compaction triggers on a token threshold and summarises older turns

**Persona**

- The persona lives in `/memory/soul.md` and is editable from the web console
- The memory module re-reads the persona and profile files on every request, so changes apply on the next turn without a reboot

**Web console**

Seven sections: status, chat, persona, memory, model, channels and network. Plain HTML with vanilla JS — no build step, bundled straight into the firmware.

## Web search

Set a Tavily or Brave API key under **Model → Web search**, save and reboot. Tavily takes priority when both are set. The model invokes `web_search` on its own, and up to three short result excerpts are fed back into the next turn.

Two things are required: the model endpoint must support function calling (the **Tool calls** switch on the same page, on by default) and at least one search key must be configured. With no key the tool is not registered and costs no context.

The C3 has no PSRAM, so a tool turn uses more heap than a plain chat. If `Out of memory` appears occasionally, shorten the persona file or use a model with a smaller context window.

## Hardware

- ESP32-C3 with 4 MB flash
- Board config `esp32_C3_4MB`
- Partitions: factory 2816 KB / system 384 KB / storage 768 KB

The current image is about 1.5 MB, using 52% of the factory partition and leaving roughly 1.3 MB free.

RAM, not flash, is the real constraint. The C3 has no PSRAM: about 262 KB of heap is available at boot, and 60–100 KB is typically usable once Wi-Fi and TLS are up. Check the headroom before adding anything that stays resident.

## Build

Requires ESP-IDF v5.5.x.

```bash
. $IDF_PATH/export.sh
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_C3_4MB
idf.py build
idf.py -p COM6 flash
```

After flashing, the board starts a provisioning hotspot named `esp-claw-XXXXXX`. Join it, open <http://192.168.4.1>, fill in Wi-Fi, the model endpoint and the QQ bot credentials, then reboot.

See [BUILD_NOTES.md](./BUILD_NOTES.md) for environment details, partition layout and the build pitfalls worth knowing.

## Upgrading

To update the application only:

```bash
idf.py -p COM6 app-flash
```

Persona, long-term memory and Wi-Fi credentials are preserved.

`idf.py flash` also overwrites the storage partition, which resets the persona and memory. Use it for first-time flashing or when the partition table changes.

## Layout

```
application/edge_agent/            main application
  boards/community/esp32_C3_4MB/   the only board definition
  components/                      app-level components: config, HTTP server, chat help
  fatfs_image/                     read-only image with default persona and memory templates
  partitions_4MB.csv               4 MB partition table
components/
  claw_capabilities/               capabilities: boards, IM channels, LLM config, sessions, system
  claw_modules/                    agent core: core, memory, event router, cap, skill
  common/                          app wiring, Wi-Fi manager, settings storage, HTTP reuse
```

## License

Apache-2.0, see [LICENSE](./LICENSE).

The on-device agent framework derives from [Espressif ESP-Claw](https://github.com/espressif/esp-claw). This project trims and rewrites it for the ESP32-C3 only and no longer tracks upstream. The original copyright notices remain in the source files.
