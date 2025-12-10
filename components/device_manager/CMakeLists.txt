idf_component_register(
    SRCS
        "device_manager.c"
        "device_manager_parse.c"
        "device_manager_validate.c"
        "device_manager_export.c"
        "profiles/dm_profiles.c"
        "storage/dm_storage.c"
        "templates/dm_templates.c"
        "runtime/dm_runtime_uid.c"
        "runtime/dm_runtime_signal.c"
        "runtime/dm_runtime_mqtt.c"
        "runtime/dm_runtime_flag.c"
        "template_registry.c"
        "template_factory.c"
        "template_applier.c"
        "template_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash json event_bus audio_player automation_engine
)
