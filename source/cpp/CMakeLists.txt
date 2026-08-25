cmake_minimum_required(VERSION 3.21)
project(substance3d_chinese_translator_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# One translation engine, two delegate binaries.  translation_ui_delegate.cpp
# is the single source of truth for both hosts:
#   * translator_delegate_qt5.dll      - Painter 7.2-10.0 (Qt 5.12.5)
#   * translator_delegate_qt6.dll      - Painter 10.1+ and Designer 14+
#     (Qt 6.x). The Designer graph-painting hooks are compiled in and enabled
#     at runtime only when the host process is Designer.
# ---------------------------------------------------------------------------
function(add_translator_delegate target output_name qt_root qt_major)
    add_library(${target} SHARED translation_ui_delegate.cpp)
    if(SP_TOOLTIP_DIAGNOSTICS)
        target_compile_definitions(${target} PRIVATE
            SD_TRANSLATION_TOOLTIP_DIAGNOSTICS)
    endif()
    target_include_directories(${target} PRIVATE
        "${qt_root}/include"
        "${qt_root}/include/QtCore"
        "${qt_root}/include/QtGui"
        "${qt_root}/include/QtWidgets"
        "${qt_root}/mkspecs/win32-msvc"
    )
    target_link_libraries(${target} PRIVATE
        "${qt_root}/lib/Qt${qt_major}Core.lib"
        "${qt_root}/lib/Qt${qt_major}Gui.lib"
        "${qt_root}/lib/Qt${qt_major}Widgets.lib"
        version.lib
        psapi.lib
    )
    target_compile_definitions(${target} PRIVATE
        QT_CORE_LIB QT_GUI_LIB QT_WIDGETS_LIB UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX /Zc:__cplusplus /permissive- /utf-8)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
    )
endfunction()

option(SP_TOOLTIP_DIAGNOSTICS
    "Enable tooltip hook diagnostics for resource hover previews" OFF)

set(QT6_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sdks/qt/6.5.3/msvc2019_64")
if(NOT EXISTS "${QT6_ROOT}/lib/Qt6Widgets.lib")
    message(FATAL_ERROR "Bundled Qt6 SDK is missing")
endif()

# Painter 10.1+ / Qt6. The Qt6 ABI remains compatible across the supported
# minor versions used by current Painter and Designer releases.
add_translator_delegate(
    translator_delegate_qt6 translator_delegate_qt6 "${QT6_ROOT}" 6)

# Painter 7.2-10.0 / Qt5.12.5. This target is deliberately a separate DLL;
# loading a Qt6-linked delegate into a Qt5 process is unsafe.
set(QT5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sdks/qt/5.12.5/msvc2017_64")
if(NOT EXISTS "${QT5_ROOT}/lib/Qt5Widgets.lib")
    message(FATAL_ERROR "Bundled Qt5.12.5 SDK is missing")
endif()
add_translator_delegate(
    translator_delegate_qt5 translator_delegate_qt5 "${QT5_ROOT}" 5)

# ---------------------------------------------------------------------------
# Standalone asset term extractor (shared by both hosts)
# ---------------------------------------------------------------------------
set(SP_DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sdks/deps")
if(NOT EXISTS "${SP_DEPS_ROOT}/lib/archive.lib")
    message(FATAL_ERROR "Bundled extractor dependencies are missing: ${SP_DEPS_ROOT}")
endif()

add_executable(translator_extractor extractor.cpp)
target_include_directories(translator_extractor PRIVATE
    "${SP_DEPS_ROOT}/include"
)
target_link_libraries(translator_extractor PRIVATE
    "${SP_DEPS_ROOT}/lib/libhdf5.lib"
    "${SP_DEPS_ROOT}/lib/archive.lib"
    "${SP_DEPS_ROOT}/lib/szip-static.lib"
    "${SP_DEPS_ROOT}/lib/aec-static.lib"
    "${SP_DEPS_ROOT}/lib/zs.lib"
    "${SP_DEPS_ROOT}/lib/bz2.lib"
    "${SP_DEPS_ROOT}/lib/lz4.lib"
    "${SP_DEPS_ROOT}/lib/lzma.lib"
    "${SP_DEPS_ROOT}/lib/zstd.lib"
    "${SP_DEPS_ROOT}/lib/libcrypto.lib"
    "${SP_DEPS_ROOT}/lib/pugixml.lib"
    shlwapi.lib
    xmllite.lib
    crypt32.lib
    ws2_32.lib
)
target_compile_definitions(translator_extractor PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(translator_extractor PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus)
    set_property(TARGET translator_extractor PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
