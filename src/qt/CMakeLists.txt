find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Concurrent DBus)
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)

include("${CMAKE_CURRENT_SOURCE_DIR}/qt-sources.cmake")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}/src/qt/forms")

add_executable(innova-qt
    ${INNOVA_QT_SOURCES}
    ${INNOVA_QT_HEADERS}
    ${INNOVA_QT_FORMS}
    "${CMAKE_SOURCE_DIR}/src/qt/bitcoin.qrc")

target_link_libraries(innova-qt PRIVATE
    innova_core
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::PrintSupport
    Qt6::Concurrent Qt6::DBus)

target_compile_definitions(innova-qt PRIVATE QT_GUI USE_QRCODE)
target_include_directories(innova-qt PRIVATE "${CMAKE_SOURCE_DIR}/src/qt")
innova_apply_common_options(innova-qt)
innova_apply_platform_definitions(innova-qt)

# Translations: compile the tracked .ts files with lrelease (qt_add_lrelease is
# only available from Qt 6.2; fall back to a manual rule otherwise).
find_program(LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt6 lrelease6)
if(LRELEASE_EXECUTABLE AND INNOVA_QT_TRANSLATIONS)
  set(_qm_files)
  foreach(_ts IN LISTS INNOVA_QT_TRANSLATIONS)
    get_filename_component(_base "${_ts}" NAME_WE)
    set(_qm "${CMAKE_CURRENT_BINARY_DIR}/${_base}.qm")
    add_custom_command(OUTPUT "${_qm}"
        COMMAND "${LRELEASE_EXECUTABLE}" "${_ts}" -qm "${_qm}"
        DEPENDS "${_ts}" COMMENT "lrelease ${_base}.ts")
    list(APPEND _qm_files "${_qm}")
  endforeach()
  add_custom_target(innova_translations ALL DEPENDS ${_qm_files})
  add_dependencies(innova-qt innova_translations)
else()
  message(WARNING "lrelease not found: Qt translations are not compiled")
endif()
