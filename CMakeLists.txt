cmake_minimum_required(VERSION 3.21)
project(TechyDraw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(techy-draw WIN32 src/main.cpp src/Canvas.cpp)
target_link_libraries(techy-draw PRIVATE Qt6::Widgets)

qt_add_executable(selftest src/selftest.cpp)
target_link_libraries(selftest PRIVATE Qt6::Gui)
