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

# The core: canvas, sheets, the plug-in host. A shared library, so the app and its plug-ins share one copy (Plugin.h)
add_library(techy-core SHARED src/Canvas.cpp src/Sheet.cpp src/Host.cpp)
target_include_directories(techy-core PUBLIC src)
target_link_libraries(techy-core PUBLIC Qt6::Widgets)
target_compile_definitions(techy-core PRIVATE TECHY_CORE)   # TECHY_DATA exports, not imports
set_target_properties(techy-core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

qt_add_executable(techy-notes WIN32 src/main.cpp)
target_link_libraries(techy-notes PRIVATE techy-core)
target_compile_definitions(techy-notes PRIVATE TECHY_VERSION="${PROJECT_VERSION}")
qt_add_resources(techy-notes icon PREFIX / FILES icon.png)   # :/icon.png, the window icon
if(WIN32)
    configure_file(src/app.rc.in app.rc @ONLY)
    target_sources(techy-notes PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)   # the .exe's icon and version
    configure_file(tools/installer.nsi.in installer.nsi @ONLY)   # the setup wizard (CI: makensis, see the script)
endif()
if(ANDROID)
    # The APK's package sources, put together in the build tree: Qt's manifest (+ keep-data question on uninstall, no permissions),
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

# Eval.cpp holds ExprTk (minutes to compile): built once, shared by the math plug-ins and the tests
add_library(math SHARED src/Math.cpp src/Eval.cpp src/Ink.cpp)
qt_add_resources(math templates PREFIX / BASE data FILES data/symbols.tpl data/inkmlp.bin)   # :/symbols.tpl, :/inkmlp.bin
target_include_directories(math PUBLIC src)
target_link_libraries(math PUBLIC Qt6::Gui)
set_target_properties(math PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(MINGW)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS -Wa,-mbig-obj)
elseif(MSVC)
    set_source_files_properties(src/Eval.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

# Plug-ins (Plugin.h): loaded at start from plugins/ next to the app
foreach(p MathGraph Recognizer)
    string(TOLOWER ${p} name)
    add_library(techyplugin-${name} MODULE plugins/${p}.cpp)
    target_link_libraries(techyplugin-${name} PRIVATE techy-core math)
    set_target_properties(techyplugin-${name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:techy-notes>/plugins)   # also with MSVC's Release/
    add_dependencies(techy-notes techyplugin-${name})   # building the app builds them
    if(ANDROID)   # into the APK's library folder, with the math library they need
        set_property(TARGET techy-notes APPEND PROPERTY QT_ANDROID_EXTRA_LIBS $<TARGET_FILE:techyplugin-${name}>)
    endif()
endforeach()
if(ANDROID)
    set_property(TARGET techy-notes APPEND PROPERTY QT_ANDROID_EXTRA_LIBS $<TARGET_FILE:math>)
endif()

if(ANDROID)
    return()   # the tools below are desktop programs (the apk target would package them too)
endif()

qt_add_executable(selftest src/selftest.cpp)
target_link_libraries(selftest PRIVATE techy-core math)

# One-off converter + classifier training, not part of the normal build: cmake --build build --target hwrt2tpl
# (it writes the data the math library embeds; the library's own resources are whatever was there before)
qt_add_executable(hwrt2tpl tools/hwrt2tpl.cpp)
target_link_libraries(hwrt2tpl PRIVATE Qt6::Gui math)
set_target_properties(hwrt2tpl PROPERTIES EXCLUDE_FROM_ALL ON)

# Simulated mouse/pen/touch input on the canvas: ./build/inputtest -platform offscreen
qt_add_executable(inputtest src/inputtest.cpp)
target_link_libraries(inputtest PRIVATE techy-core Qt6::GuiPrivate math)
