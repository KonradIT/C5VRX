# UART video link to a LilyGo T-Embed: the C5VRX receiver (root main/)
# without its analog output, plus components/video_link_uart (found
# automatically), which grabs frames from the receiver's raw I/Q ring and
# streams them over UART. See README.md.
cmake_minimum_required(VERSION 3.16)
get_filename_component(C5VRX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# PlatformIO passes EXTRA_COMPONENT_DIRS itself (its src_dir is the root
# main/), so append; a component listed twice is harmless.
list(APPEND EXTRA_COMPONENT_DIRS "${C5VRX_ROOT}/main")

set(SDKCONFIG_DEFAULTS "${C5VRX_ROOT}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(VIDEO_LINK_DEBUG)
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.debug.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Selects the link hooks in main/video.c (no PARLIO TX, no menu raster).
idf_build_set_property(COMPILE_DEFINITIONS "C5VRX_VIDEO_LINK_EXPERIMENT=1" APPEND)

project(c5vrx_video_link)
