# ESP32C3-chatbot

基于 Espressif ESP-Claw 的轻量 QQ 聊天机器人，面向 4 MB Flash 的 ESP32-C3。

- 使用已配置的模型进行 QQ 文字聊天。
- 在网页上设置 Wi-Fi、兼容 OpenAI 接口的模型和 QQ 机器人。
- 聊天记录保存在板子上，重启后可以续聊。回忆用户给过的内容时，模型会收到优先查阅用户原话的指示。

## 编译和设置

启用 ESP-IDF v5.5.4 环境并安装 ESP Board Manager。首次安装时，从克隆下来的仓库执行以下命令。

```bash
pip install esp-bmgr-assist
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_C3_4MB
idf.py build
idf.py -p <port> flash monitor
```

把 `<port>` 换成 C3 的串口，例如 Windows 上的 `COM7`。连接串口日志显示的设备设置热点，打开设置页填写 Wi-Fi、模型和 QQ 机器人配置，保存后重启。[设备设置](http://192.168.4.1/)

如果板子已经安装同一分区布局的 C3 固件，编译后运行 `idf.py -p <port> app-flash` 即可更新应用并保留设置和聊天记录。

[English](./README.md) · [编译说明](./docs/src/content/docs/zh-cn/reference-project/build-from-source.mdx) · [ESP32-S3 项目](https://github.com/Bigesila-B/esp-claw-chatbot)

## 上游项目和许可证

本项目保留了 Espressif ESP-Claw 的源码历史和 Apache-2.0 许可证。[ESP-Claw 上游](https://github.com/espressif/esp-claw) · [许可证](./LICENSE)
