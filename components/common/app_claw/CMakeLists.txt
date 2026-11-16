
set(app_claw_srcs
    "app_claw.c"
    "app_capabilities.c"
)

if(CONFIG_APP_CLAW_CAP_LUA OR NOT CONFIG_APP_CLAW_TEXT_ONLY)
    list(APPEND app_claw_srcs "app_lua_modules.c")
endif()

if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND app_claw_srcs "app_claw_cli.c")
endif()

idf_component_register(
    SRCS
        ${app_claw_srcs}
    INCLUDE_DIRS
        "include"
)
