idf_component_register(
    SRCS "app_chat_help.c" "device_status.c" "token_usage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES claw_cap claw_core claw_event_router esp_system esp_timer heap nvs_flash
)
