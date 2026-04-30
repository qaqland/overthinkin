cmake_minimum_required(VERSION 3.16)
project(keyi VERSION 0.3.2 LANGUAGES C)

if(APPLE)
    message(FATAL_ERROR "Not supported on macOS.")
endif()

include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(keyi keyi.c)

target_compile_definitions(keyi PRIVATE PROG_VERSION="${PROJECT_VERSION}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(keyi PRIVATE -Wall -Wextra)
else()
    target_compile_definitions(keyi PRIVATE NDEBUG)
endif()

install(
    TARGETS keyi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
