QT += widgets charts core network
TARGET = wifiperson
TEMPLATE = app
CONFIG += c++17

linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += libnl-3.0 libnl-genl-3.0
}

SOURCES += main.cpp \
    mainwindow.cpp \
    wifi_scanner.cpp

linux {
    SOURCES += wifi_scanner_linux.cpp
}

win32 {
    SOURCES += wifi_scanner_windows.cpp
    LIBS += -lwlanapi
    DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
}

HEADERS += mainwindow.h \
    wifi_scanner_backend.h \
    wifi_scanner.h \
    wifi_types.h

isEmpty(PREFIX): PREFIX = /usr
target.path = $$PREFIX/bin
INSTALLS += target
