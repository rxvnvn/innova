find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport Concurrent DBus)
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)

include("${CMAKE_CURRENT_SOURCE_DIR}/qt-sources.cmake")

set(CMAKE_AUTOMOC ON)
# AUTOUIC is deliberately OFF: several Qt sources include the core header
# "ui_interface.h" (src/ui_interface.h, the wallet/UI notification interface).
# With AUTOUIC on, the generator maps that include to a form named
# interface.ui, which does not and should not exist. Forms are wrapped
# explicitly below with qt6_wrap_ui() instead - deterministic and readable.
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}/src/qt/forms")

# The proven qmake configuration (innova-qt.pro lines 605/629/710) also compiles
# these three translation units into the GUI: init.cpp (AppInit2, HelpMessage,
# the argument globals - its main() is excluded by #ifndef QT_GUI), noui.cpp
# (the non-GUI notification stubs the wallet expects) and kernelrecord.cpp
# (the Qt PoS kernel model).
set(INNOVA_QT_CORE_EXTRA
    "${CMAKE_SOURCE_DIR}/src/init.cpp"
    "${CMAKE_SOURCE_DIR}/src/noui.cpp"
    "${CMAKE_SOURCE_DIR}/src/kernelrecord.cpp")


# These seven sources include "ui_interface.h", which is the CORE header
# src/ui_interface.h (the wallet notification interface), not a generated form.
# AUTOUIC would map that include to a form named interface.ui, which does not and
# should not exist, so uic scanning is skipped for them. Every real form is still
# processed by AUTOUIC.
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/src/qt/addeditadrenalinenode.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/clientmodel.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/walletmodel.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/transactiondesc.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/qtipcserver.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/transactiontablemodel.cpp"
    "${CMAKE_SOURCE_DIR}/src/qt/bitcoin.cpp"
    "${CMAKE_SOURCE_DIR}/src/init.cpp"
    "${CMAKE_SOURCE_DIR}/src/noui.cpp"
    "${CMAKE_SOURCE_DIR}/src/kernelrecord.cpp"
    PROPERTIES SKIP_AUTOUIC ON)

add_executable(innova-qt
    ${INNOVA_QT_SOURCES}
    ${INNOVA_QT_CORE_EXTRA}
    ${INNOVA_QT_HEADERS}
    ${INNOVA_QT_FORMS}
    "${CMAKE_SOURCE_DIR}/src/qt/bitcoin.qrc"
    )

target_link_libraries(innova-qt PRIVATE
    innova_core
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::PrintSupport
    Qt6::Concurrent Qt6::DBus)

target_compile_definitions(innova-qt PRIVATE QT_GUI USE_QRCODE)
target_include_directories(innova-qt PRIVATE
    "${CMAKE_SOURCE_DIR}/src/qt"
    # generated ui_*.h from qt6_wrap_ui() live in the Qt binary dir
    "${CMAKE_CURRENT_BINARY_DIR}")
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

# QR encoder required by USE_QRCODE / qrcodedialog.cpp. The vendored copy
# (src/qrcode) is no longer present in the tree, so the system libqrencode is
# used instead. QR encoding is cosmetic (address QR codes), and the dependency is
# optional: without it the GUI still builds, minus the QR dialog.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(QRENCODE QUIET libqrencode)
endif()
if(QRENCODE_FOUND)
  target_link_libraries(innova-qt PRIVATE ${QRENCODE_LIBRARIES})
  target_include_directories(innova-qt PRIVATE ${QRENCODE_INCLUDE_DIRS})
  target_compile_options(innova-qt PRIVATE ${QRENCODE_CFLAGS_OTHER})
  message(STATUS "innova-qt: QR codes via system libqrencode ${QRENCODE_VERSION}")
else()
  message(WARNING "innova-qt: libqrencode not found - USE_QRCODE will fail to link")
endif()
