# Third-party dependencies as imported targets.
# Boost / OpenSSL / BerkeleyDB / curl / libevent / zlib / MiniUPnPc come from the
# system. LevelDB is bundled in src/leveldb and built by its own makefile (the
# upstream project ships no CMake build); the result is imported here.

set(Boost_USE_STATIC_LIBS OFF)
find_package(Boost 1.60 REQUIRED COMPONENTS
             filesystem program_options thread chrono unit_test_framework)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

find_path(DB_CXX_INCLUDE_DIR db_cxx.h)
find_library(DB_CXX_LIBRARY NAMES db_cxx)
if(NOT DB_CXX_INCLUDE_DIR OR NOT DB_CXX_LIBRARY)
  message(FATAL_ERROR "BerkeleyDB (db_cxx) not found: install libdb++-dev / libdb5.3++-dev")
endif()
add_library(innova::bdb UNKNOWN IMPORTED)
set_target_properties(innova::bdb PROPERTIES
  IMPORTED_LOCATION "${DB_CXX_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${DB_CXX_INCLUDE_DIR}")

find_path(EVENT_INCLUDE_DIR event2/event.h)
find_library(EVENT_LIBRARY NAMES event)
if(NOT EVENT_INCLUDE_DIR OR NOT EVENT_LIBRARY)
  message(FATAL_ERROR "libevent not found: install libevent-dev")
endif()
add_library(innova::event UNKNOWN IMPORTED)
set_target_properties(innova::event PROPERTIES
  IMPORTED_LOCATION "${EVENT_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${EVENT_INCLUDE_DIR}")

find_library(MINIUPNPC_LIBRARY NAMES miniupnpc)
if(NOT MINIUPNPC_LIBRARY)
  message(WARNING "MiniUPnPc not found: building with -DINNOVA_USE_UPNP=OFF")
  set(INNOVA_USE_UPNP OFF CACHE BOOL "" FORCE)
else()
  add_library(innova::miniupnpc UNKNOWN IMPORTED)
  set_target_properties(innova::miniupnpc PROPERTIES IMPORTED_LOCATION "${MINIUPNPC_LIBRARY}")
endif()

# ---- bundled LevelDB -------------------------------------------------------
set(LEVELDB_DIR "${CMAKE_SOURCE_DIR}/src/leveldb")
set(LEVELDB_LIB "${LEVELDB_DIR}/libleveldb.a")
set(MEMENV_LIB  "${LEVELDB_DIR}/libmemenv.a")
add_custom_command(
  OUTPUT "${LEVELDB_LIB}" "${MEMENV_LIB}"
  COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
          make -j4 libleveldb.a libmemenv.a
  WORKING_DIRECTORY "${LEVELDB_DIR}"
  COMMENT "Building bundled LevelDB"
  VERBATIM)
add_custom_target(innova_leveldb DEPENDS "${LEVELDB_LIB}" "${MEMENV_LIB}")
add_library(innova::leveldb UNKNOWN IMPORTED)
set_target_properties(innova::leveldb PROPERTIES IMPORTED_LOCATION "${LEVELDB_LIB}")
add_library(innova::memenv UNKNOWN IMPORTED)
set_target_properties(innova::memenv PROPERTIES IMPORTED_LOCATION "${MEMENV_LIB}")
add_dependencies(innova::leveldb innova_leveldb)
add_dependencies(innova::memenv innova_leveldb)
