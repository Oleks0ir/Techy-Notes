cmake_minimum_required(VERSION 3.21)
project(TechyNotes VERSION 0.2.1 LANGUAGES CXX)   # the release version: About, .exe properties, installer, APK
if(WIN32)
    enable_language(RC)   # src/app.rc.in: the .exe icon and version
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(QT_NO_PRIVATE_MODULE_WARNING ON)   # inputtest fakes touch points through QMutableEventPoint
find_package(Qt6 REQUIRED COMPONENTS Widgets Network GuiPrivate)   # Network: QLocalServer, one app at a time
qt_standard_project_setup()

qt_add_executable(techy-notes WIN32 src/main.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(techy-notes PRIVATE Qt6::Widgets math)
target_compile_definitions(techy-notes PRIVATE TECHY_VERSION="${PROJECT_VERSION}")
qt_add_resources(techy-notes icon PREFIX / FILES icon.png)   # :/icon.png, the window icon
if(WIN32)
    configure_file(src/app.rc.in app.rc @ONLY)
    target_sources(techy-notes PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)   # the .exe's icon and version
    configure_file(tools/installer.nsi.in installer.nsi @ONLY)   # the setup wizard (CI: makensis, see the script)
endif()
if(ANDROID)
    # The APK's package sources, put together in the build tree: Qt's manifest (+ keep-data question on uninstall),
    # the icon, the license texts (CI adds the LGPL). ponytail: arm64-v8a only (tablets); add ABIs when a device needs one
    set(pkg ${CMAKE_CURRENT_BINARY_DIR}/android)
    file(COPY android/AndroidManifest.xml DESTINATION ${pkg})
    file(COPY icon.png DESTINATION ${pkg}/res/drawable)
    file(COPY LICENSE data/LICENSE-HWRT.txt DESTINATION ${pkg}/assets/licenses)
    math(EXPR code "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_target_properties(techy-notes PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${pkg}
        QT_ANDROID_PACKAGE_NAME io.github.oleks0ir.techynotes
        QT_ANDROID_APP_NAME Techy-Notes
        QT_ANDROID_APP_ICON @drawable/icon
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE ${code})
else()
    target_link_libraries(techy-notes PRIVATE Qt6::Network)   # Android keeps one instance itself; no INTERNET permission
endif()

# Eval.cpp holds ExprTk (minutes to compile): built once, linked everywhere
add_library(math STATIC src/Math.cpp src/Eval.cpp src/Ink.cpp)
qt_add_resources(math templates PREFIX / BASE data FILES data/symbols.tpl data/inkmlp.bin)   # :/symbols.tpl, :/inkmlp.bin
target_link_libraries(math PUBLIC Qt6::Gui)
if(MINGW)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS -Wa,-mbig-obj)
elseif(MSVC)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

if(ANDROID)
    return()   # the tools below are desktop programs (the apk target would package them too)
endif()

qt_add_executable(selftest src/selftest.cpp src/Sheet.cpp)
target_link_libraries(selftest PRIVATE Qt6::Gui math)

# One-off converter + classifier training, not part of the normal build: cmake --build build --target hwrt2tpl
# (it writes the data the math library embeds; the library's own resources are whatever was there before)
qt_add_executable(hwrt2tpl tools/hwrt2tpl.cpp)
target_link_libraries(hwrt2tpl PRIVATE Qt6::Gui math)
set_target_properties(hwrt2tpl PROPERTIES EXCLUDE_FROM_ALL ON)

# Simulated mouse/pen/touch input on the canvas: ./build/inputtest -platform offscreen
qt_add_executable(inputtest src/inputtest.cpp src/Canvas.cpp src/Sheet.cpp)
target_link_libraries(inputtest PRIVATE Qt6::Widgets Qt6::GuiPrivate math)
