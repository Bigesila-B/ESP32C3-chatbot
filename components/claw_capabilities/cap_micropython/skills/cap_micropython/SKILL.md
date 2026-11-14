---
{
  "name": "cap_micropython",
  "description": "Run .py scripts on the ESP32-S3 with MicroPython, pass JSON arguments, and read printed results.",
  "metadata": {
    "manage_mode": "readonly",
    "cap_groups": ["cap_micropython"]
  }
}
---

# MicroPython scripts

Use `python_run_script` to run a UTF-8 `.py` file under the SYSTEM or DATA filesystem. For a script bundled in a Skill, use its `{CUR_SKILL_DIR}/scripts/...` path.

Pass an optional JSON object as `args`. The script receives it in a global `args` dictionary and can return text through `print()`. The default `timeout_ms` is 1000 and the maximum is 5000. Each file can contain up to 32 KiB. The tool captures up to 4095 bytes of printed output and reports truncation.

The embedded runtime provides built-ins including `gc`, `json`, `math`, and `sys`, plus imports of `.py` modules under the script's directory. Use the existing ESP-Claw capabilities or Lua modules for network and device I/O.

For a quick check, run the bundled example with these arguments.

~~~json
{"path":"{CUR_SKILL_DIR}/scripts/hello.py","args":{"name":"ESP32-S3","count":4}}
~~~

The script prints a JSON object with the MicroPython implementation name, greeting, and sum.
