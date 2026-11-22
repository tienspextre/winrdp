cmake_minimum_required(VERSION 3.22)
project(winrdp VERSION 1.1.5 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FREERDP3_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freerdp3-install")
set(FREERDP3_LIB_DIR "${FREERDP3_INSTALL_DIR}/lib")
set(FREERDP3_PKGCONFIG_DIR "${FREERDP3_LIB_DIR}/pkgconfig")

if(EXISTS "${FREERDP3_PKGCONFIG_DIR}")
    if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        set(ENV{PKG_CONFIG_PATH} "${FREERDP3_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${FREERDP3_PKGCONFIG_DIR}")
    endif()
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FREERDP3 REQUIRED IMPORTED_TARGET freerdp3>=3.33.1 freerdp-client3>=3.33.1 winpr3>=3.33.1)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(X11LIBS REQUIRED IMPORTED_TARGET x11 xrandr)

add_library(winrdp_core STATIC
    src/config/profile.cpp
    src/config/rdp_file.cpp
    src/config/keyring.cpp
    src/rdp/framebuffer.cpp
    src/input/monitor_math.cpp
    src/input/scancode_map.cpp
)

target_include_directories(winrdp_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${FREERDP3_INCLUDE_DIRS}
)

target_compile_options(winrdp_core
    PRIVATE -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(winrdp_core
    PUBLIC
        PkgConfig::CJSON
        PkgConfig::SECRET
        Threads::Threads
)

add_executable(winrdp
    src/main.cpp
    src/app/application.cpp
    src/rdp/pointer_bridge.cpp
    src/rdp/clipboard_bridge.cpp
    src/rdp/rdp_session.cpp
    src/render/gl_canvas.cpp
    src/input/monitor_layout.cpp
    src/input/input_manager.cpp
    src/ui/connection_bar.cpp
    src/ui/cert_auth_dialogs.cpp
    src/ui/session_window.cpp
    src/ui/profile_dialog.cpp
    src/ui/manager_window.cpp
)

target_include_directories(winrdp
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${FREERDP3_INCLUDE_DIRS}
)

target_compile_definitions(winrdp PRIVATE WINRDP_VERSION="${PROJECT_VERSION}")

target_compile_options(winrdp
    PRIVATE -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(winrdp
    PRIVATE
        winrdp_core
        PkgConfig::FREERDP3
        PkgConfig::GTK4
        PkgConfig::ADW
        PkgConfig::EPOXY
        PkgConfig::X11LIBS
        Threads::Threads
)

set_target_properties(winrdp PROPERTIES
    BUILD_RPATH "${FREERDP3_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib/winrdp/lib"
    BUILD_WITH_INSTALL_RPATH FALSE
)

target_link_options(winrdp PRIVATE "-Wl,--disable-new-dtags")

install(TARGETS winrdp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/style.css DESTINATION ${CMAKE_INSTALL_DATADIR}/winrdp)
install(FILES data/io.winrdp.Client.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/winrdp-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)

enable_testing()

macro(winrdp_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE winrdp_core)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

winrdp_add_test(test_rdp_file tests/test_rdp_file.cpp)
winrdp_add_test(test_profile tests/test_profile.cpp)
winrdp_add_test(test_framebuffer tests/test_framebuffer.cpp)
winrdp_add_test(test_input_mapping tests/test_input_mapping.cpp)
winrdp_add_test(test_monitor_layout tests/test_monitor_layout.cpp)
