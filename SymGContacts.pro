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

TARGET = GoogleSync
TEMPLATE = app

SOURCES += main.cpp syncmanager.cpp \
    localsslproxy.cpp
HEADERS += \
    syncmanager.h \
    localsslproxy.h
OTHER_FILES += startup.rss

INCLUDEPATH += $$PWD
CONFIG += PIPS
INCLUDEPATH += $$EPOCROOT/epoc32/include/libc
INCLUDEPATH += $$EPOCROOT/epoc32/include/libc/sys

symbian {
    TARGET.UID3 = 0xE3CB3BE0 # Замените на ваш UID
    #TARGET.CAPABILITY = ReadUserData WriteUserData NetworkServices ReadDeviceData WriteDeviceData
TARGET.CAPABILITY = NetworkServices
    TARGET.CAPABILITY += LocalServices
    TARGET.CAPABILITY += Location
    TARGET.CAPABILITY += UserEnvironment
    TARGET.CAPABILITY += SwEvent
    TARGET.CAPABILITY +=  SurroundingsDD
    TARGET.CAPABILITY +=  ProtServ
    TARGET.CAPABILITY +=  PowerMgmt
    TARGET.CAPABILITY +=  ReadDeviceData
    TARGET.CAPABILITY +=  WriteDeviceData
    TARGET.CAPABILITY +=  TrustedUI
    TARGET.CAPABILITY +=  NetworkControl
    TARGET.CAPABILITY +=  MultimediaDD
    TARGET.CAPABILITY +=  CommDD
    TARGET.CAPABILITY +=  DiskAdmin
    #load(data_caging_paths)
    TARGET.CAPABILITY += ReadUserData
    TARGET.CAPABILITY += WriteUserData

    TARGET.EPOCHEAPSIZE = 0x40000 0x4000000

    # Подключаем нативные библиотеки Symbian для работы с контактами
    LIBS += -lPbkEng -lcntmodel -leuser -lbafl -lmbedtls

    # Настройка автозапуска (копирование RSS файла в системную папку загрузки)
    startup_res.sources = startup.rss
    startup_res.path = /private/101f875a/import/[0xE3CB3BE0].rsc # Имя файла должно совпадать с UID3 без 0x
    DEPLOYMENT += startup_res

# Please do not modify the following two lines. Required for deployment.
include(qmlapplicationviewer/qmlapplicationviewer.pri)
qtcAddDeployment()

}

RESOURCES += \
    res.qrc
