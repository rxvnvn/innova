include("${CMAKE_CURRENT_SOURCE_DIR}/core-sources.cmake")

# ---------------------------------------------------------------------------
# innova_core: static library with the whole production source graph.
# The daemon main(), the Qt main() and the Boost test main() are NOT here.
# ---------------------------------------------------------------------------
add_library(innova_core STATIC ${INNOVA_CORE_SOURCES})

target_include_directories(innova_core PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/json"
    "${CMAKE_SOURCE_DIR}/src/chainstate"
    "${CMAKE_SOURCE_DIR}/src/chainstate/store3a"
    "${CMAKE_SOURCE_DIR}/src/minizip"
    "${BUILD_H_DIR}"
    "${LEVELDB_DIR}/include"
    "${LEVELDB_DIR}"
    "${LEVELDB_DIR}/helpers")

target_compile_definitions(innova_core PUBLIC
    BOOST_SPIRIT_THREADSAFE
    USE_LEVELDB
    HAVE_BUILD_INFO
    $<$<BOOL:${INNOVA_USE_UPNP}>:USE_UPNP=1>)

target_link_libraries(innova_core PUBLIC
    Boost::filesystem
    Boost::program_options
    Boost::thread
    Boost::chrono
    OpenSSL::Crypto
    OpenSSL::SSL
    innova::bdb
    innova::event
    innova::leveldb
    innova::memenv
    CURL::libcurl
    ZLIB::ZLIB)

if(INNOVA_USE_UPNP)
  target_link_libraries(innova_core PUBLIC innova::miniupnpc)
endif()

if(INNOVA_USE_NATIVETOR)
  target_sources(innova_core PRIVATE
      "${CMAKE_SOURCE_DIR}/src/tor/tor_main.c"
      "${CMAKE_SOURCE_DIR}/src/ed25519/donna/ed25519_tor.c")
  target_compile_definitions(innova_core PUBLIC USE_NATIVETOR=1)
  target_include_directories(innova_core PUBLIC "${CMAKE_SOURCE_DIR}/src/tor")
endif()

innova_apply_common_options(innova_core)
innova_apply_platform_definitions(innova_core)
add_dependencies(innova_core build_h)

# ---------------------------------------------------------------------------
# innovad (daemon + CLI): the production entry point lives in init.cpp.
# ---------------------------------------------------------------------------
if(INNOVA_BUILD_DAEMON)
  add_executable(innovad "${CMAKE_SOURCE_DIR}/src/init.cpp")
  target_link_libraries(innovad PRIVATE innova_core)
  innova_apply_common_options(innovad)
  innova_apply_platform_definitions(innovad)
endif()

# ---------------------------------------------------------------------------
# Supported CLI tools. Each one is a thin main() over innova_core.
# ---------------------------------------------------------------------------
if(INNOVA_BUILD_TOOLS)
  set(INNOVA_TOOLS
      blockindex-recovery-tool:src/blockindex_recovery_tool_cli.cpp
      blockindex-catchup-tool:src/blockindex_catchup_tool_cli.cpp
      innova-blockindex-builder:src/blockindex_generation_builder_tool.cpp
      innova-blockindex-builder-lm:src/blockindex_generation_builder_lm_tool.cpp
      innova-txloc-build:src/txloc_build_tool.cpp
      innova-lm-verify:src/lm_verify_tool.cpp
      innova-undo-replay:src/undo_replay_tool.cpp
      innova-lm-fixture:src/lm_fixture_tool.cpp
      innova-blockindex-navdiff:src/blockindex_navigation_diff_tool.cpp)
  foreach(_t IN LISTS INNOVA_TOOLS)
    string(REPLACE ":" ";" _pair "${_t}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _src)
    add_executable(${_name} "${CMAKE_SOURCE_DIR}/${_src}")
    target_link_libraries(${_name} PRIVATE innova_core)
    innova_apply_common_options(${_name})
    innova_apply_platform_definitions(${_name})
  endforeach()
endif()

if(INNOVA_BUILD_QT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/qt/CMakeLists.txt")
  add_subdirectory(qt)
endif()

if(INNOVA_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory(test)
endif()
