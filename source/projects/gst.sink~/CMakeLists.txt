include(${MAX_SDK_BASE_SCRIPT_DIR}/max-pretarget.cmake)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum OS X deployment version" FORCE)

include_directories(
    "${MAX_SDK_INCLUDES}"
    "${MAX_SDK_MSP_INCLUDES}"
    "${MAX_SDK_JIT_INCLUDES}"
)

get_filename_component(COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../common" ABSOLUTE)

set(PROJECT_SRC
    "gst.sink~.cpp"
    "${COMMON_DIR}/gst_audio_support.cpp"
)

add_library(
    ${PROJECT_NAME}
    MODULE
    ${PROJECT_SRC}
)

target_include_directories(${PROJECT_NAME} PRIVATE "${COMMON_DIR}")
configure_gstreamer_target(${PROJECT_NAME})

include(${MAX_SDK_BASE_SCRIPT_DIR}/max-posttarget.cmake)
