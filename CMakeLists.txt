cmake_minimum_required(VERSION 3.21)
project(msTCTextureStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC flags
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(_UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Qt6 ──────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools)
qt_standard_project_setup()

# ── vcpkg packages ───────────────────────────────────────────────
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)

# ── Sources ──────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/Theme.h
    src/MainWindow.h
    src/MainWindow.cpp

    src/pmx/PmxTypes.h
    src/pmx/PmxParser.h
    src/pmx/PmxParser.cpp

    src/renderer/DX11Widget.h
    src/renderer/DX11Widget.cpp
    src/renderer/Camera.h
    src/renderer/Camera.cpp
    src/renderer/Shaders.h
    src/renderer/StaticMesh.h
    src/renderer/StaticMesh.cpp
    src/renderer/MaskTextureSet.h
    src/renderer/MaskTextureSet.cpp
    src/renderer/MeshRaycast.h
    src/renderer/MeshRaycast.cpp

    src/editor/TextureDocument.h
    src/editor/TextureDocument.cpp
    src/editor/TextureExport.h
    src/editor/TextureExport.cpp
    src/editor/TextureInfo.h
    src/editor/TextureInfo.cpp
    src/editor/TextureGroup.h
    src/editor/TextureGroup.cpp
    src/editor/PixelTarget.h
    src/editor/Layer.h
    src/editor/Layer.cpp
    src/editor/LayerStack.h
    src/editor/LayerStack.cpp
    src/editor/UndoHistory.h
    src/editor/UndoHistory.cpp
    src/editor/LayerPanel.h
    src/editor/LayerPanel.cpp
    src/editor/ChannelColors.h
    src/editor/UvLines.h
    src/editor/UvLines.cpp
    src/editor/UvCoverage.h
    src/editor/UvCoverage.cpp
    src/editor/FloodFill.h
    src/editor/FloodFill.cpp
    src/editor/CanvasWidget.h
    src/editor/CanvasWidget.cpp
    src/editor/BrushEngine.h
    src/editor/BrushEngine.cpp
    src/editor/BrushPanel.h
    src/editor/BrushPanel.cpp
    src/editor/InfoPanel.h
    src/editor/InfoPanel.cpp

    src/project/Project.h
    src/project/Project.cpp
)

# ── Executable ───────────────────────────────────────────────────
qt_add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Stb_INCLUDE_DIR}
)

# ── Link ─────────────────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    fmt::fmt
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

# ── Translations ─────────────────────────────────────────────────
# Compile .ts → .qm at build time and copy alongside the executable.
# At runtime main.cpp picks the right .qm based on the current locale
# (or a saved user preference).
set(TS_FILES
    translations/mstc_zh_CN.ts
)
qt_add_translations(${PROJECT_NAME}
    TS_FILES        ${TS_FILES}
    QM_FILES_OUTPUT_VARIABLE QM_FILES
    LUPDATE_OPTIONS  -no-obsolete
)
# Stage compiled .qm files in build/<cfg>/translations/ so QTranslator
# can find them via QCoreApplication::applicationDirPath() + "/translations".
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/translations
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${QM_FILES}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/translations
    COMMENT "Staging Qt translation .qm files"
)
