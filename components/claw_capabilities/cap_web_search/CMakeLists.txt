# 联网搜索能力：支持 Brave 与 Tavily 两种 REST 服务商。
# 上游还带一个 CLI 命令文件（cmd_cap_web_search.c），依赖 esp_console；
# 精简构建没有控制台，这里不编入，省下 flash 和一次无用初始化。
idf_component_register(
    SRCS
        "src/cap_web_search.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        esp_http_client
        http_reuse
)
