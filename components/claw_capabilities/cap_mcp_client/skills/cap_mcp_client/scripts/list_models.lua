-- SPDX-License-Identifier: Apache-2.0
local capability = require("capability")
local json = require("json")
local options = args or {}
local titles = { image = "图片模型", video = "视频模型", all = "模型" }
local kind = options.kind or "all"
local offset = math.tointeger(options.offset == "" and 0 or options.offset or 0)

if not titles[kind] or not offset or offset < 0 then
    print("用法：/mcp models [起始位置]")
    return
end

local page_buffer = 16 * 1024
local function directory_schema()
    local cursor
    for _ = 1, 4 do
        local ok, text = capability.call("mcp_list_tools", {
            server = options.server, tool_name = "list_models", cursor = cursor,
        }, { max_output_bytes = page_buffer })
        if not ok or type(text) ~= "string" or text:sub(1, 6) == "Error:" then
            return nil, text or "无法读取 MCP 工具列表。"
        end
        local encoded = text:match("\nInput schema: ([^\n]+)")
        if encoded then
            local decoded, schema = pcall(json.decode, encoded)
            if decoded and type(schema) == "table" then
                return schema
            end
            return nil, "模型目录的参数格式无法识别。"
        end
        cursor = text:match("%(nextCursor: (.*)%)$")
        if not cursor then
            break
        end
    end
    return nil, "当前 MCP 服务未公开 list_models 目录工具。可在 MCP 设置中测试连接并查看可用工具。"
end

local schema, schema_error = directory_schema()
if not schema then
    print(schema_error)
    return
end

local properties = type(schema.properties) == "table" and schema.properties or {}
local params = {}
for key, value in pairs({ kind = kind, query = options.query, supports_reference = options.supports_reference }) do
    if properties[key] then
        params[key] = value
    end
end
local min_limit = 1
if properties.limit then
    local limit = type(properties.limit) == "table" and properties.limit or {}
    min_limit = math.tointeger(limit.minimum) or 1
    params.limit = math.max(min_limit, math.min(3, math.tointeger(limit.maximum) or 3))
end
if properties.offset then
    params.offset = offset
elseif offset > 0 then
    print("当前模型目录不支持从指定位置查询。可发送 /mcp models 查看完整结果。")
    return
end
for _, key in ipairs(type(schema.required) == "table" and schema.required or {}) do
    if params[key] == nil then
        print("当前模型目录需要参数 " .. key .. "。可在聊天中指定服务名和所需参数。")
        return
    end
end

local function fetch_page()
    local ok, text, err = capability.call("mcp_call_tool", {
        server = options.server, tool_name = "list_models", arguments = params,
    }, { max_output_bytes = page_buffer })
    if not ok then
        return nil, err or "请求失败"
    end
    local decoded, page = pcall(json.decode, text or "")
    if decoded and type(page) == "table" then
        local items = page.items or page.models or page.data or (text:match("^%s*%[") and page)
        if type(items) == "table" then
            return { items = items, total = type(page.total) == "number" and page.total or nil,
                next_offset = properties.offset and page.next_offset or nil }
        end
    end
    return nil, text and #text >= page_buffer - 4 and "ESP_ERR_INVALID_SIZE" or "目录响应无效"
end

local lines, used, total, notice = {}, 0, nil, nil
while offset do
    if properties.offset then
        params.offset = offset
    end
    local page, err = fetch_page()
    if not page and err == "ESP_ERR_INVALID_SIZE" and params.limit and params.limit > min_limit then
        params.limit = min_limit
        page, err = fetch_page()
    end
    if not page then
        notice = "目录查询在位置 " .. offset .. " 失败（" .. err .. "）。"
        break
    end
    total = page.total
    for index, item in ipairs(page.items) do
        local key = type(item) == "table" and (item.key or item.id or item.model or item.name) or item
        if type(key) ~= "string" or key == "" then
            notice = "服务返回了无效的模型 ID。"
            break
        end
        local label = type(item) == "table" and (item.label or item.name) or key
        local line = "- " .. (type(label) == "string" and label or key) .. " [" .. key .. "]"
        if type(item) == "table" and item.ready ~= true then
            line = line .. (item.ready == false and "（不可用）" or "（状态未知）")
        end
        if used + #line + 1 > 1650 then
            notice = properties.offset and "继续查询请用 /mcp models " .. (offset + index - 1) or "当前回复已达到长度上限。"
            break
        end
        lines[#lines + 1] = line
        used = used + #line + 1
    end
    if notice then
        break
    end
    local next_offset = page.next_offset
    if next_offset ~= nil and (type(next_offset) ~= "number" or not math.tointeger(next_offset) or
        next_offset <= offset or #page.items == 0) then
        notice = "服务返回了无效的下一页位置。"
        break
    end
    offset = next_offset
end

table.insert(lines, 1, "MCP " .. (properties.kind and titles[kind] or titles.all) .. " · 本次列出 " .. #lines .. " 个" ..
    (total and "，目录共 " .. total .. " 个" or ""))
if notice then
    lines[#lines + 1] = notice
end
print(table.concat(lines, "\n"))
