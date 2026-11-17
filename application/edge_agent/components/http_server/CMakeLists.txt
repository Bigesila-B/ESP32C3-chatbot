set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_ICO  "${FRONTEND_SOURCE_DIR}/public/favicon.ico")

# 本项目只面向 ESP32-C3 精简版，前端固定使用 frontend_text/index.html。
# 原先这里有一段「React 产物缺失就 FATAL_ERROR」的检查，但组件 CMakeLists 在
# 配置阶段会先以空 CONFIG_ 执行一遍，导致精简构建被误判并中断，因此移除。

set(http_server_srcs
    "http_server_core.c"
    "http_server_assets.c"
    "http_server_config_api.c"
    "http_server_status_api.c"
    "http_server_netdiag_api.c"
    "http_server_chat_api.c"
    "http_server_session_api.c"
    "http_server_json.c"
    "http_server_utils.c"
)

set(http_server_requires
    app_config
    app_chat_help
    app_claw
    claw_cap
    claw_utils
    esp_http_server
    esp_http_client
    heap
)

# 记忆管理接口在精简版同样需要，放在 TEXT_ONLY 分支之外
if(CONFIG_APP_CLAW_CAP_MEMORY)
    list(APPEND http_server_srcs "http_server_memory_api.c")
    list(APPEND http_server_requires claw_memory)
endif()

if(CONFIG_APP_CLAW_TEXT_ONLY)
    set(http_server_assets "${CMAKE_CURRENT_LIST_DIR}/frontend_text/index.html")
else()
    set(http_server_assets "${FRONTEND_DIST_HTML_GZ}" "${FRONTEND_PUBLIC_FAVICON_ICO}")
    list(APPEND http_server_srcs
        "http_server_capabilities_api.c"
        "http_server_lua_modules_api.c"
        "http_server_chat_help_api.c"
        "http_server_skill_catalog_api.c"
        "http_server_files_api.c"
        "http_server_wechat_api.c"
        "http_server_webim_api.c"
    )
endif()

if(CONFIG_APP_CLAW_CAP_SKILL_MGR)
    list(APPEND http_server_requires claw_skill)
endif()

if(CONFIG_APP_CLAW_CAP_LUA AND CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER AND NOT CONFIG_APP_CLAW_TEXT_ONLY)
    list(APPEND http_server_srcs "http_server_lua_app_api.c")
    list(APPEND http_server_requires lua_module_http_server)
endif()

if(CONFIG_APP_CLAW_CAP_MCP_CLIENT)
    list(APPEND http_server_srcs "http_server_mcp_api.c")
endif()

idf_component_register(
    SRCS
        ${http_server_srcs}
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        ${http_server_assets}
    REQUIRES
        ${http_server_requires}
)

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${http_server_assets})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
