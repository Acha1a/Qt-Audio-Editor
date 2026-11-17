QT       += core gui
QT += core gui widgets multimedia


greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
CONFIG += c++17 link_pkgconfig
CONFIG += link_pkgconfig
PKGCONFIG += gstreamer-1.0
PKGCONFIG += gstreamer-1.0 gstreamer-app-1.0
PKGCONFIG += gstreamer-audio-1.0


INCLUDEPATH += C:/msys64/mingw64/include/gstreamer-1.0
INCLUDEPATH += C:/msys64/mingw64/include/glib-2.0
INCLUDEPATH += C:/msys64/mingw64/lib/glib-2.0/include
INCLUDEPATH += C:/msys64/mingw64/include/gstreamer-1.0/gst

LIBS += -LC:/msys64/mingw64/lib -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lgstvideo-1.0


# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h
    audiowidget.h

FORMS += \
    mainwindow.ui

# CONFIG(debug, debug | release) {
#     DESTDIR = $$OUT_PWD/debug
# } else {
#     DESTDIR = $$OUT_PWD/release
# }

# QMAKE_POST_LINK = $$(QTDIR)/bin/windeployqt.exe $$shell_quote($$DESTDIR/$$shell_quote($$TARGET).exe) --no-compiler-runtime

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
