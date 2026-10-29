cmake_minimum_required(VERSION 3.20)

project(InnovaCore
        VERSION 5.0.5
        DESCRIPTION "Innova Core - Tribus PoW/PoS hybrid cryptocurrency"
        LANGUAGES C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(INNOVA_BUILD_DAEMON "Build innovad" ON)
option(INNOVA_BUILD_QT     "Build the Qt GUI (requires Qt6 Widgets)" OFF)
option(INNOVA_BUILD_TESTS  "Build test_innova and register CTest suites" ON)
option(INNOVA_BUILD_TOOLS  "Build the supported CLI tools" ON)
option(INNOVA_USE_UPNP     "Enable UPnP support" ON)
option(INNOVA_USE_NATIVETOR "Enable the bundled NativeTor integration" OFF)

include(CompilerOptions)
include(PlatformOptions)
include(Dependencies)

# build.h (version metadata) - generated, never committed
set(BUILD_H_DIR "${CMAKE_BINARY_DIR}/obj")
file(MAKE_DIRECTORY "${BUILD_H_DIR}")
add_custom_command(
  OUTPUT "${BUILD_H_DIR}/build.h"
  COMMAND sh "${CMAKE_SOURCE_DIR}/share/genbuild.sh" "${BUILD_H_DIR}/build.h"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src"
  COMMENT "Generating build.h (version metadata)"
  VERBATIM)
add_custom_target(build_h DEPENDS "${BUILD_H_DIR}/build.h")

add_subdirectory(src)

enable_testing()

install(TARGETS innovad RUNTIME DESTINATION bin OPTIONAL)
