cmake_minimum_required(VERSION 3.21)
project(anycubic_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

qt_add_executable(anycubic-viewer
    main.cpp
    main_window.cpp
    main_window.hpp
    print_view.cpp
    print_view.hpp
    scene.cpp
    scene.hpp
    support_preview.cpp
    support_preview.hpp
    icons.qrc)
target_link_libraries(anycubic-viewer PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(anycubic-viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(anycubic-viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../target/release")
