# Add more folders to ship with the application, here
#folder_01.source = qml
#$folder_01.target = qml
DEPLOYMENTFOLDERS = folder_01

# Additional import path used to resolve QML modules in Creator's code model
QML_IMPORT_PATH =

# Smart Installer package's UID
# This UID is from the protected range and therefore the package will
# fail to install if self-signed. By default qmake uses the unprotected
# range value if unprotected UID is defined for the application and
# 0x2002CCCF value if protected UID is given to the application
#symbian:DEPLOYMENT.installer_header = 0x2002CCCF

QT += core gui network script declarative

TARGET = SymGContacts
TEMPLATE = app

SOURCES += main.cpp syncmanager.cpp filepicker.cpp
HEADERS += syncmanager.h filepicker.h

OTHER_FILES += startup.rss

INCLUDEPATH += $$PWD
CONFIG += PIPS
INCLUDEPATH += $$EPOCROOT/epoc32/include/libc
INCLUDEPATH += $$EPOCROOT/epoc32/include/libc/sys

INCLUDEPATH += $$EPOCROOT/epoc32/include/app
INCLUDEPATH += $$EPOCROOT/epoc32/include/platform

symbian {
    TARGET.UID3 = 0xE3CB3BE0
    TARGET.CAPABILITY = NetworkServices ReadUserData WriteUserData

    TARGET.EPOCHEAPSIZE = 0x40000 0x4000000

    LIBS +=  -lcntmodel -leuser -lbafl -lmbedtls
    icon.files = SymGContacts.png
    icon.path = /
    DEPLOYMENT += icon

    #startup_res.sources = startup.rss
    #startup_res.path = /private/101f875a/import/[0xE3CB3BE0].rsc # Имя файла должно совпадать с UID3 без 0x
    #DEPLOYMENT += startup_res
    MMP_RULES += "DEBUGGABLE_UDEBONLY"
    MMP_RULES += "SRCDBG"

# Please do not modify the following two lines. Required for deployment.
include(qmlapplicationviewer/qmlapplicationviewer.pri)
qtcAddDeployment()

}

RESOURCES += \
    res.qrc
