# ESP32C3-chatbot

A lightweight QQ chatbot for an ESP32-C3 with 4 MB of flash, based on Espressif's ESP-Claw.

- QQ text chat with the existing model provider.
- Web settings for Wi-Fi, an OpenAI-compatible model, and the QQ bot.
- On-device chat history for continuing a conversation after reboot. When recalling a value you provided, the model is instructed to use your original message as the source.

## Build and set up

Activate ESP-IDF v5.5.4 and install ESP Board Manager. Run these commands from the cloned repository for a first-time installation.

```bash
pip install esp-bmgr-assist
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_C3_4MB
idf.py build
idf.py -p <port> flash monitor
```

Replace `<port>` with the C3 serial port, for example `COM7` on Windows. Join the setup hotspot shown in the serial log and open the device settings. Enter the Wi-Fi, model, and QQ bot settings, then save and restart. [Device settings](http://192.168.4.1/)

To update an existing C3 installation with the same partition table, run `idf.py -p <port> app-flash` after building. This preserves settings and chat history.

[简体中文](./README_CN.md) · [Build guide](./docs/src/content/docs/en/reference-project/build-from-source.mdx) · [ESP32-S3 project](https://github.com/Bigesila-B/esp-claw-chatbot)

## Upstream and license

This project retains Espressif's ESP-Claw source history and Apache-2.0 license. [ESP-Claw upstream](https://github.com/espressif/esp-claw) · [License](./LICENSE)
