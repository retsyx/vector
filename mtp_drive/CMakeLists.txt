cmake_minimum_required(VERSION 3.18)
project(mtp_drive LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "mtp_drive requires macOS")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMTP REQUIRED libmtp)
pkg_check_modules(MACFUSE QUIET IMPORTED_TARGET fuse3>=3.18.3)
if(NOT MACFUSE_FOUND)
    message(FATAL_ERROR "Install macFUSE 5.5 or later from https://macfuse.io. If necessary, add /usr/local/lib/pkgconfig to PKG_CONFIG_PATH.")
endif()

add_executable(mtp_drive main.cpp filesystem.cpp mtp_device.cpp)
target_compile_features(mtp_drive PRIVATE cxx_std_17)
set_target_properties(mtp_drive PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(mtp_drive PRIVATE -Wall -Wextra -Wpedantic)
# Homebrew's libmtp.pc places a linker-only IOKit flag in Cflags.
target_include_directories(mtp_drive SYSTEM PRIVATE ${LIBMTP_INCLUDE_DIRS})
find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
target_link_libraries(mtp_drive PRIVATE ${LIBMTP_LINK_LIBRARIES} PkgConfig::MACFUSE ${COREFOUNDATION_LIBRARY})
