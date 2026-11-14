# MicroPython scripts for ESP-Claw Skills

Build for an ESP32-S3 with PSRAM. In `idf.py menuconfig`, enable **App Claw Config → App Claw Capabilities → Enable MicroPython scripts for Skills** and run `idf.py build`. CMake fetches the pinned MicroPython v1.28.0 source. To use an existing checkout, pass `-D FETCHCONTENT_SOURCE_DIR_CLAW_MICROPYTHON=/path/to/micropython` with commit `e0e9fbb17ed6fd06bb76e266ae554784c9c80804`.

The firmware includes the `cap_micropython` Skill in the SYSTEM image. Activating it makes `python_run_script` available to the current Agent session. Add `"cap_groups": ["cap_micropython"]` to a custom Skill's metadata to expose the same tool. Pass the bundled script's `{CUR_SKILL_DIR}/scripts/...` path and an optional JSON `args` object. The script reads the global `args` dictionary and returns text with `print()`.

From the device console, verify the bundled example with these commands.

~~~text
skill --activate cap_micropython --session cli
cap call python_run_script "{\"path\":\"/system/skills/cap_micropython/scripts/hello.py\",\"args\":{\"count\":7}}"
~~~

The example returns `"runtime": "micropython"` and `"sum": 28`. Each call creates a fresh VM with a 256 KiB PSRAM heap and a 16 KiB task stack. The runner includes `gc`, `io` in-memory streams, `json`, `math`, and `sys`. It can import source modules from the script directory. Use ESP-Claw capabilities or Lua modules for hardware and network operations.
