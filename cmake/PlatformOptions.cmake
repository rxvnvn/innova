# Per-platform definitions and system libraries. Nothing here is global: every
# target opts in through innova_apply_platform_definitions().

find_package(Threads REQUIRED)

function(innova_apply_platform_definitions target)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE
        WIN32 _WIN32_WINNT=0x0601 WINVER=0x0601
        _MT $<$<CONFIG:Debug>:_DEBUG>)
    target_link_libraries(${target} PRIVATE ws2_32 mswsock shlwapi iphlpapi
        crypt32 userenv advapi32)
  elseif(APPLE)
    target_compile_definitions(${target} PRIVATE MAC_OSX)
    target_link_libraries(${target} PRIVATE "-framework CoreFoundation"
        "-framework Foundation" "-framework ApplicationServices")
  else()
    target_compile_definitions(${target} PRIVATE LINUX)
    target_link_libraries(${target} PRIVATE dl)
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
      target_compile_definitions(${target} PRIVATE BSD)
    endif()
  endif()

  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(${target} PRIVATE IS_ARCH_64)
  endif()

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm.*)$")
    set(_scrypt "${CMAKE_SOURCE_DIR}/src/scrypt-arm.S")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set(_scrypt "${CMAKE_SOURCE_DIR}/src/scrypt-x86.S")
  else()
    set(_scrypt "${CMAKE_SOURCE_DIR}/src/scrypt-x86_64.S")
  endif()
  target_sources(${target} PRIVATE "${_scrypt}")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(${target} PRIVATE -g)
  endif()
endfunction()
