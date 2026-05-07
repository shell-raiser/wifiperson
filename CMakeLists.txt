cmake_minimum_required(VERSION 3.16)
project(WiFiperson VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Network)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-3.0 libnl-genl-3.0)
endif()

if(WIN32)
    set(APP_EXECUTABLE_TYPE WIN32)
    set(WIFI_SCANNER_BACKEND_SOURCE wifi_scanner_windows.cpp)
elseif(UNIX AND NOT APPLE)
    set(WIFI_SCANNER_BACKEND_SOURCE wifi_scanner_linux.cpp)
else()
    message(FATAL_ERROR "Wi-Fi scanning backend is not implemented for this platform.")
endif()

add_executable(wifiperson ${APP_EXECUTABLE_TYPE}
    main.cpp
    mainwindow.h
    mainwindow.cpp
    wifi_scanner_backend.h
    wifi_scanner.h
    wifi_scanner.cpp
    ${WIFI_SCANNER_BACKEND_SOURCE}
    wifi_types.h
)

target_link_libraries(
    wifiperson
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Charts
        Qt6::Network
)

if(TARGET PkgConfig::LIBNL)
    target_link_libraries(wifiperson PRIVATE PkgConfig::LIBNL)
endif()

if(WIN32)
    target_compile_definitions(wifiperson PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(wifiperson PRIVATE Wlanapi)
endif()

set_target_properties(wifiperson PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

install(TARGETS wifiperson RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX AND NOT APPLE)
    install(FILES packaging/io.github.wifiperson.wifiperson.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/wifiperson.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
