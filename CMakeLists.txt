cmake_minimum_required(VERSION 3.20)

project(SimpleUI
    VERSION 0.1.0
    DESCRIPTION "Declarative desktop UI engine for non-web developers"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)

add_library(sui STATIC
    Core/Object/Object.cpp
    Core/Resources/Resources.cpp
    Core/Application/Application.cpp
    Core/Application/PaintOrder.cpp
    SUI/Markup/Parser.cpp
    SUI/Script/Script.cpp
    SUI/Script/Timers.cpp
    SUI/DOM/Element.cpp
    SUI/DOM/Document.cpp
    SUI/DOM/MutationObserver.cpp
    CSS/DefaultStyle.cpp
    CSS/Easing.cpp
    CSS/Animation.cpp
    CSS/Parser.cpp
    CSS/StyleResolver.cpp
    CSS/PseudoElements.cpp
    Layout/BlockLayout.cpp
    Core/Application/Background.cpp
    Core/Application/FontFaces.cpp
    Rendering/ImageCache.cpp
    Rendering/ImageBlur.cpp
    Rendering/ClipMask.cpp
    Rendering/ColorMatrix.cpp
    Rendering/SvgMarker.cpp
    Rendering/SvgPaint.cpp
    Rendering/SvgText.cpp
    Rendering/Tessellate.cpp
    Rendering/Renderer.cpp
    Rendering/Text.cpp
    Platform/Platform.cpp
    Platform/Linux/LinuxPlatform.cpp)

target_include_directories(sui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sui PUBLIC PkgConfig::SDL3 PkgConfig::HARFBUZZ PkgConfig::FRIBIDI)
target_compile_options(sui PRIVATE -Wall -Wextra)

# stb_truetype is a header-only dependency; make it available via include path.
target_include_directories(sui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# The template is also the worked example of the script model: each <script> in
# template/index.sui names a translation unit that claims its href via
# SUI_SCRIPT, so the build has to compile them into the executable.
add_executable(template
    template/main.cpp
    template/src/loading.cpp
    template/src/calculate.cpp)
target_link_libraries(template PRIVATE sui)
target_include_directories(template PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/template/src)

enable_testing()
add_subdirectory(tests)