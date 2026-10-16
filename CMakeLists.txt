cmake_minimum_required(VERSION 3.21)
project(TechyNotes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QT_NO_PRIVATE_MODULE_WARNING ON)   # inputtest fakes touch points through QMutableEventPoint
find_package(Qt6 REQUIRED COMPONENTS Widgets GuiPrivate)
qt_standard_project_setup()

qt_add_executable(techy-notes WIN32 src/main.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(techy-notes PRIVATE Qt6::Widgets)

qt_add_executable(selftest src/selftest.cpp src/Sheet.cpp)
target_link_libraries(selftest PRIVATE Qt6::Gui)

# Simulated mouse/pen/touch input on the canvas: ./build/inputtest -platform offscreen
qt_add_executable(inputtest src/inputtest.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(inputtest PRIVATE Qt6::Widgets Qt6::GuiPrivate)
