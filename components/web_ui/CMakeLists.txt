idf_component_register(
    SRCS
        "web_ui_pictures.c"
        "web_ui_utils.c"
        "web_ui_devices.c"
        "web_ui_page.c"
        "web_ui_laser.c"
        "web_ui_robot.c"
        "web_ui.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES
    "assets/devices_wizard.css"
    "assets/devices_wizard.js"
    "assets/devices_editor.css"
    "assets/devices_editor.js"
    REQUIRES esp_http_server event_bus config_store network audio_player mqtt_core device_manager automation_engine
)
