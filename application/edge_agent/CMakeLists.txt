cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.2.1")
project(edge_agent)

fatfs_create_spiflash_image(storage fatfs_image FLASH_IN_PROJECT)

skill_builder_configure_skill_sync(
    TARGET fatfs_storage_bin
    SKILL_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/fatfs_image/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_storage_bin
    BUILTIN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/fatfs_image/scripts/builtin"
    DOCS_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/fatfs_image/scripts/docs"
)
