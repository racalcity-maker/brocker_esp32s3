set(WIZARD_PARTS
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_01_prelude.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_02_state.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_03_render.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_04_helpers.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_05_mutations.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_06_wizard.js
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/wizard/wizard_07_footer.js
)

set(GENERATED_WIZARD_JS ${CMAKE_CURRENT_SOURCE_DIR}/assets/devices_wizard.js)

if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT EXISTS ${GENERATED_WIZARD_JS})
        file(WRITE ${GENERATED_WIZARD_JS} "")
    endif()
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
        OUTPUT ${GENERATED_WIZARD_JS}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/assets/build_devices_wizard.py
                ${GENERATED_WIZARD_JS} ${WIZARD_PARTS}
        DEPENDS ${WIZARD_PARTS} ${CMAKE_CURRENT_SOURCE_DIR}/assets/build_devices_wizard.py
        COMMENT "Bundling devices_wizard.js"
    )
    add_custom_target(devices_wizard_bundle ALL DEPENDS ${GENERATED_WIZARD_JS})
endif()

idf_component_register(
    SRCS
        "web_ui_utils.c"
        "web_ui_devices.c"
        "web_ui_page.c"
        "web_ui.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES
        "assets/devices_wizard.css"
        ${GENERATED_WIZARD_JS}
        "assets/devices_editor.css"
        "assets/devices_editor.js"
    REQUIRES esp_http_server event_bus config_store network audio_player mqtt_core device_manager automation_engine
)

if(TARGET devices_wizard_bundle)
    add_dependencies(${COMPONENT_LIB} devices_wizard_bundle)
endif()
