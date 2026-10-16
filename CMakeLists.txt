cmake_minimum_required(VERSION 3.21)
project(TechyNotes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QT_NO_PRIVATE_MODULE_WARNING ON)   # inputtest fakes touch points through QMutableEventPoint
find_package(Qt6 REQUIRED COMPONENTS Widgets GuiPrivate)
qt_standard_project_setup()

qt_add_executable(techy-notes WIN32 src/main.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(techy-notes PRIVATE Qt6::Widgets math)

# Eval.cpp holds ExprTk (minutes to compile): built once, linked everywhere
add_library(math STATIC src/Math.cpp src/Eval.cpp src/Ink.cpp)
qt_add_resources(math templates PREFIX / BASE data FILES data/symbols.tpl)   # :/symbols.tpl
target_link_libraries(math PUBLIC Qt6::Gui)
if(MINGW)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS -Wa,-mbig-obj)
elseif(MSVC)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

qt_add_executable(selftest src/selftest.cpp src/Sheet.cpp)
target_link_libraries(selftest PRIVATE Qt6::Gui math)

# One-off converter, not part of the normal build: cmake --build build --target hwrt2tpl
qt_add_executable(hwrt2tpl tools/hwrt2tpl.cpp)
target_link_libraries(hwrt2tpl PRIVATE math)
set_target_properties(hwrt2tpl PROPERTIES EXCLUDE_FROM_ALL ON)

# Simulated mouse/pen/touch input on the canvas: ./build/inputtest -platform offscreen
qt_add_executable(inputtest src/inputtest.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(inputtest PRIVATE Qt6::Widgets Qt6::GuiPrivate math)
