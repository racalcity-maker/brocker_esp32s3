set(TEST_SRCS
    "test_runner.c"
    "../../../components/device_manager/test/test_device_manager_parse.c"
)

idf_component_register(
    SRCS ${TEST_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES device_manager unity json
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/../../components/device_manager"
)
