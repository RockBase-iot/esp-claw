idf_component_register(
    SRCS
        "http_server_core.c"
        "http_server_assets.c"
        "http_server_capabilities_api.c"
        "http_server_config_api.c"
        "http_server_lua_modules_api.c"
        "http_server_status_api.c"
        "http_server_files_api.c"
        "http_server_wechat_api.c"
        "http_server_json.c"
        "http_server_utils.c"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "web/index.html"
        "web/styles.css"
        "web/app.js"
        "web/lean-qr.min.mjs"
    REQUIRES
        app_config
        app_claw
        esp_http_server
        heap
        json
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
