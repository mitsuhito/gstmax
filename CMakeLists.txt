cmake_minimum_required(VERSION 3.19)

project(gstmax LANGUAGES C CXX)

set(CMAKE_XCODE_GENERATE_SCHEME YES)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "macOS architecture" FORCE)
endif()

option(EXPORT_COMPILE_COMMANDS "Export compile commands for LSP" ON)

if(EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

function(resolve_max_sdk_base_dir output_var candidate_path)
    if(EXISTS "${candidate_path}/c74support/max-includes/ext.h")
        set(${output_var} "${candidate_path}" PARENT_SCOPE)
    elseif(EXISTS "${candidate_path}/source/max-sdk-base/c74support/max-includes/ext.h")
        set(${output_var} "${candidate_path}/source/max-sdk-base" PARENT_SCOPE)
    else()
        set(${output_var} "${candidate_path}" PARENT_SCOPE)
    endif()
endfunction()

set(GSTMAX_DEFAULT_MAX_SDK_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source/max-sdk-base")

if(DEFINED ENV{MAX_SDK_BASE_DIR} AND NOT "$ENV{MAX_SDK_BASE_DIR}" STREQUAL "")
    set(GSTMAX_DEFAULT_MAX_SDK_BASE_DIR "$ENV{MAX_SDK_BASE_DIR}")
elseif(DEFINED ENV{MAX_SDK_ROOT} AND NOT "$ENV{MAX_SDK_ROOT}" STREQUAL "")
    set(GSTMAX_DEFAULT_MAX_SDK_BASE_DIR "$ENV{MAX_SDK_ROOT}")
endif()

set(MAX_SDK_BASE_DIR "${GSTMAX_DEFAULT_MAX_SDK_BASE_DIR}" CACHE PATH "Path to the max-sdk-base checkout")
resolve_max_sdk_base_dir(GSTMAX_RESOLVED_MAX_SDK_BASE_DIR "${MAX_SDK_BASE_DIR}")
set(MAX_SDK_BASE_DIR "${GSTMAX_RESOLVED_MAX_SDK_BASE_DIR}" CACHE PATH "Path to the max-sdk-base checkout" FORCE)

set(C74_SUPPORT_DIR "${MAX_SDK_BASE_DIR}/c74support")
set(MAX_SDK_BASE_SCRIPT_DIR "${MAX_SDK_BASE_DIR}/script")
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/externals")

if(NOT EXISTS "${C74_SUPPORT_DIR}/max-includes/ext.h")
    message(FATAL_ERROR "Max SDK is missing. Run scripts/setup_max_sdk.sh first.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)
pkg_get_variable(GSTMAX_GSTREAMER_PRIMARY_LIBDIR gstreamer-1.0 libdir)

set(GSTMAX_GSTREAMER_RPATHS "")
foreach(directory IN LISTS GSTREAMER_LIBRARY_DIRS)
    if(IS_ABSOLUTE "${directory}" AND NOT directory STREQUAL GSTMAX_GSTREAMER_PRIMARY_LIBDIR)
        list(APPEND GSTMAX_GSTREAMER_RPATHS "${directory}")
    endif()
endforeach()
list(REMOVE_DUPLICATES GSTMAX_GSTREAMER_RPATHS)

set(PACKAGE_VERSION "0.1.0")
set(GIT_VERSION_TAG "${PACKAGE_VERSION}")
set(AUTHOR_DOMAIN "dev.codex")
set(COPYRIGHT_STRING "Copyright (c) 2026 dev")
set(BUNDLE_IDENTIFIER "\${PRODUCT_NAME:rfc1034identifier}")
set(EXCLUDE_FROM_COLLECTIVES "no")

function(configure_gstreamer_target target_name)
    target_link_libraries(${target_name} PRIVATE PkgConfig::GSTREAMER)
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/common")

    if(APPLE AND GSTMAX_GSTREAMER_RPATHS)
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "${GSTMAX_GSTREAMER_RPATHS}"
        )
    endif()
endfunction()

macro(subdirlist result curdir)
    file(GLOB children RELATIVE "${curdir}" "${curdir}/*")
    set(dirlist "")

    foreach(child ${children})
        if(IS_DIRECTORY "${curdir}/${child}")
            list(APPEND dirlist "${child}")
        endif()
    endforeach()

    set(${result} "${dirlist}")
endmacro()

subdirlist(PROJECT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/source/projects")

foreach(project_dir IN LISTS PROJECT_DIRS)
    set(project_path "${CMAKE_CURRENT_SOURCE_DIR}/source/projects/${project_dir}")

    if(EXISTS "${project_path}/CMakeLists.txt")
        add_subdirectory("${project_path}")
    endif()
endforeach()
