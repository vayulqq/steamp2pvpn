cmake_minimum_required(VERSION 3.20)
project(SteamP2PVPN CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "Данный проект предназначен для сборки через MSVC под Windows.")
endif()

# Пути к внешним библиотекам (задайте свои при необходимости)
set(STEAMWORKS_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "Path to Steamworks SDK")
set(IMGUI_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui" CACHE PATH "Path to Dear ImGui")

# Загрузка GLFW
find_package(glfw3 REQUIRED)

# Исходные файлы ImGui
set(IMGUI_SOURCES
    ${IMGUI_PATH}/imgui.cpp
    ${IMGUI_PATH}/imgui_draw.cpp
    ${IMGUI_PATH}/imgui_tables.cpp
    ${IMGUI_PATH}/imgui_widgets.cpp
    ${IMGUI_PATH}/backends/imgui_impl_glfw.cpp
    ${IMGUI_PATH}/backends/imgui_impl_opengl3.cpp
)

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/wintun_manager.h
    src/wintun_manager.cpp
    src/steam_tunnel.h
    src/steam_tunnel.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${STEAMWORKS_SDK_PATH}/public
    ${IMGUI_PATH}
    ${IMGUI_PATH}/backends
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${STEAMWORKS_SDK_PATH}/redistributable_bin/win64
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    opengl32.lib
    ws2_32.lib
    iphlpapi.lib
    steam_api64.lib
)

# Автоматическое копирование системных бинарников
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${STEAMWORKS_SDK_PATH}/redistributable_bin/win64/steam_api64.dll"
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
