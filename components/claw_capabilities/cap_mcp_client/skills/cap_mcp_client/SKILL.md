---
{
  "name": "cap_mcp_client",
  "description": "查看已配置的 MCP、列出工具和可用的图片或视频模型、调用 MCP 生图或生成视频。Discover the configured MCP, list its image/video models, and call tools.",
  "metadata": {
    "manage_mode": "readonly",
    "category": ["network"],
    "peripherals": []
  }
}
---

# 使用已配置的 MCP

当用户问「你可以看到我配置的 MCP 吗」，先用 `mcp_list_servers({})` 查看已保存的名称和默认服务。要列出某个服务的工具，用 `mcp_list_tools` 的 `server` 参数传入名称。省略名称时使用默认服务。根据实际查询结果回答连接状态和工具名称。

若工具不可用，引导用户到「Capabilities 管理」为 MCP Client 勾选「LLM 可见」，保存并重启。鉴权失败时，引导用户在网页的「MCP 设置」检查令牌。对话中只报告连接状态、工具名或错误类型。

用 `mcp_list_tools` 的 `tool_name` 查看所选工具的输入和输出结构，再用 `mcp_call_tool` 传入相同的 `server`、工具名和参数。工具名和参数由各服务定义。

## 发送工具返回的图片

用户要求生成或发送一张图片时，给 `mcp_call_tool` 加上 `"image_delivery":{}`，让设备处理直接返回的图片。工具有异步结果时，按实际声明填入 `status_tool`（只读查询工具）、`id_field`（任务编号字段）、`id_argument`（查询参数），需要时设置 `status_field` 和 `image_field`。字段支持点路径如 `task.id`。服务明确要求新建幂等编号时，可以给出 `request_id_argument`。

收到设备接管任务的提示后，告诉用户出图后会发到此聊天。其他异步工具的进度用同一服务提供的查询工具检查。用户要重发近期图片时，调用 `mcp_last_image({})` 或使用 `/mcp last-image`，根据实际状态回应。

## 查询图片和视频模型

服务提供 `list_models` 时，可用 `lua_run_script` 读取精简目录。用户问图片模型时传 `{"kind":"image"}`，视频传 `{"kind":"video"}`，两类都问时传 `{"kind":"all"}`。查询指定服务时也在脚本的 `args` 里传入 `server`。

`{"path":"{CUR_SKILL_DIR}/scripts/list_models.lua","args":{"kind":"image"}}`

脚本按声明填写筛选和分页参数，也支持简单的模型数组。接到继续位置时，沿用筛选条件并传入该 `offset`。若服务使用其他目录工具，可按它的定义调用。由选中的生图工具定义模型、提示词和其他入参。

Use `mcp_list_servers({})` to discover saved names. Use the same `server` when listing tools, inspecting a schema, and calling a named server. Report actual connection and tool results.
