cmake_minimum_required(VERSION 3.20)
project(sync-music-player VERSION 0.0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE APP_VERSION="${PROJECT_VERSION}")
