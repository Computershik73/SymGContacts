import QtQuick 1.0

Rectangle {
    width: 360
    height: 640
    color: "#001e36" // Глубокий синий (Windows/Metro стиль)

    Column {
        anchors.centerIn: parent
        spacing: 20
        width: parent.width * 0.9

        Text {
            text: "Синхронизация Google"
            font.pixelSize: 26
            color: "white"
            font.bold: true
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Rectangle {
            width: parent.width
            height: 40
            color: "white"
            radius: 5
            TextInput {
                id: clientIdInput
                anchors.fill: parent
                anchors.margins: 5
                font.pixelSize: 16
                text: "152824189610-qr6octjqoo9elmreo6463a3qf8l4uqjl.apps.googleusercontent.com"
                color: "black"
                selectByMouse: true
            }
        }

        Rectangle {
            width: parent.width
            height: 40
            color: "white"
            radius: 5
            TextInput {
                id: clientSecretInput
                anchors.fill: parent
                anchors.margins: 5
                font.pixelSize: 16
                text: "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"
                color: "black"
                selectByMouse: true
            }
        }

        Rectangle {
            id: authCodeContainer
            width: parent.width
            height: 80
            color: "#333333"
            visible: false
            radius: 5
            Column {
                anchors.centerIn: parent
                Text { text: "Введите код в браузере:"; color: "white"; font.pixelSize: 14 }
                Text { id: authCodeText; text: "ABCD"; color: "#ff9900"; font.pixelSize: 24; font.bold: true }
            }
        }

        Rectangle {
            id: syncBtn
            width: parent.width
            height: 50
            color: "#0078D7"
            radius: 5
            Text {
                text: "Войти и Синхронизировать"
                color: "white"
                font.pixelSize: 18
                anchors.centerIn: parent
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    syncBtn.opacity = 0.5
                    statusText.text = "Запуск..."
                    // Вызов C++ метода
                    syncManager.startAuthAndSync(clientIdInput.text, clientSecretInput.text);
                }
            }
        }

        Text {
            id: statusText
            text: ""
            color: "#00ff00"
            font.pixelSize: 16
            wrapMode: Text.WordWrap
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Соединяем сигналы из C++ с функциями в QML
    Connections {
        target: syncManager
        onProgressUpdated: {
            statusText.text = message;
        }
        onAuthCodeReceived: {
            authCodeContainer.visible = true;
            authCodeText.text = userCode;
        }
        onSyncFinished: {
            statusText.text = message;
            syncBtn.opacity = 1.0;
            authCodeContainer.visible = false;
        }
    }
}
