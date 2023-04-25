cmake_minimum_required(VERSION 3.16.0)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components/esp-idf-lib/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartmatrix-idf)

# Create a LittleFS image from the contents of the 'fs_image' directory
# that fits the partition named 'littlefs'. The generated image should be flashed
# when the entire project is flashed to the target with 'idf.py -p PORT flash'.
littlefs_create_partition_image(littlefs fs_image FLASH_IN_PROJECT)