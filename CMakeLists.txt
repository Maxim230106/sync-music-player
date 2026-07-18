cmake_minimum_required(VERSION 3.21)
project(sync-music-player VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia)

set(SYNC_MUSIC_PLAYER_RESOURCE_SOURCES)
set(SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS ON)
set(SYNC_MUSIC_PLAYER_REQUIRED_ASSETS
    "${CMAKE_SOURCE_DIR}/assets/sync-music-player_logo.png"
    "${CMAKE_SOURCE_DIR}/assets/chevron-down-dark.svg"
    "${CMAKE_SOURCE_DIR}/assets/chevron-down-light.svg"
)

foreach(asset_file IN LISTS SYNC_MUSIC_PLAYER_REQUIRED_ASSETS)
    if(NOT EXISTS "${asset_file}")
        set(SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS OFF)
        break()
    endif()
endforeach()

if(SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS)
    list(APPEND SYNC_MUSIC_PLAYER_RESOURCE_SOURCES resources/assets.qrc)
endif()

set(SYNC_MUSIC_PLAYER_EXECUTABLE_TYPE)
if(WIN32)
    set(SYNC_MUSIC_PLAYER_EXECUTABLE_TYPE WIN32)
endif()

qt_add_executable(${PROJECT_NAME}
    ${SYNC_MUSIC_PLAYER_EXECUTABLE_TYPE}
    src/main.cpp
    src/session_controller.cpp
    src/ui/main_window.cpp
    include/session_controller.hpp
    include/ui/main_window.hpp
    ${SYNC_MUSIC_PLAYER_RESOURCE_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)

target_compile_definitions(${PROJECT_NAME} PRIVATE APP_VERSION="${PROJECT_VERSION}")

if(SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE SYNC_MUSIC_PLAYER_HAS_EMBEDDED_ASSETS=0)
    message(STATUS "Optional UI assets were not found. Building without embedded logo and SVG icons.")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/music"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/music"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/music"
)
