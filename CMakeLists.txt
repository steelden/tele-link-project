# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "TeleLink Desktop - MTS Link Chats client"
    HOMEPAGE_URL "https://desktop.telegram.org"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)

# TeleLink: the compiler processes of a project are limited. /MP without the
# number runs one for each logical processor, with the precompiled headers
# they exceed the memory (C1076, C3859). 4 GB for each process by default.
if (MSVC)
    cmake_host_system_information(RESULT total_memory_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT logical_cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR default_processes "${total_memory_mb} / 4096")
    if (default_processes GREATER logical_cores)
        set(default_processes ${logical_cores})
    endif()
    if (default_processes LESS 1)
        set(default_processes 1)
    endif()
    set(TELELINK_COMPILE_PROCESSES ${default_processes} CACHE STRING
        "The compiler processes of a project (/MP), 4 GB of the memory each")
    get_target_property(common_compile_options common_options INTERFACE_COMPILE_OPTIONS)
    list(TRANSFORM common_compile_options
        REPLACE "^/MP$" "/MP${TELELINK_COMPILE_PROCESSES}")
    set_target_properties(common_options PROPERTIES
        INTERFACE_COMPILE_OPTIONS "${common_compile_options}")
    message(STATUS "TeleLink: ${TELELINK_COMPILE_PROCESSES} compiler processes of a project")
endif()

include(cmake/external/qt/package.cmake)

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)
add_subdirectory(Telegram)
