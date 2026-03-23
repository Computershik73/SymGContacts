import QtQuick 1.0

Rectangle {
    id: root
    width: 360
    height: 640
    color: "#001e36"
	
	property bool isPortrait: root.width < root.height
    
    // Делаем состояние реактивным
    property bool isLoggedIn: syncManager.hasToken()
    
    Column {
        anchors.centerIn: parent
		
		spacing: root.isPortrait ? 20 : 10 
        width: parent.width * 0.9

        Text {
            text: "SymGContacts"
            font.pixelSize: 26
            color: "white"
            font.bold: true
            anchors.horizontalCenter: parent.horizontalCenter
        }
		
		Text {
            text: "by Computershik @ 4pda"
            font.pixelSize: 20
            color: "white"
            font.bold: true
            anchors.horizontalCenter: parent.horizontalCenter
        }

        // --- БЛОК ВХОДА (показывается, если НЕ залогинены) ---
        Column {
            width: parent.width
            spacing: 20
            visible: !root.isLoggedIn
            
            Rectangle { width: parent.width; height: 40; color: "white"; radius: 5
                TextInput { id: clientIdInput; anchors.fill: parent; anchors.margins: 5; font.pixelSize: 16; text: 
""; color: "black"; }
            }
            Rectangle { width: parent.width; height: 40; color: "white"; radius: 5
                TextInput { id: clientSecretInput; anchors.fill: parent; anchors.margins: 5; font.pixelSize: 16; text: 
""; color: "black"; }
            }
            Rectangle {
                id: syncBtn
                width: parent.width; height: 50; color: "#0078D7"; radius: 5
                Text { text: "Войти и Синхронизировать"; color: "white"; font.pixelSize: 18; anchors.centerIn: parent }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.startAuthAndSync(clientIdInput.text, clientSecretInput.text)
                }
            }
			
			Rectangle {
            id: authCodeContainer
            width: parent.width
            height: 120
            color: "#333333"
            visible: false // Скрыт до получения кода
            radius: 5
            Column {
                anchors.centerIn: parent
                Text { text: "Код для Google:"; color: "white"; font.pixelSize: 14 }
                Text { id: authCodeText; text: "---"; color: "#ff9900"; font.pixelSize: 24; font.bold: true }
                Text { text: "Введите этот код в браузере в google.com/device"; wrapMode: Text.Wrap; color: "#aaaaaa"; font.pixelSize: 12 }
            }
			}
        }

        // --- БЛОК СИНХРОНИЗАЦИИ (показывается, если ЗАЛОГИНЕНЫ) ---
        Column {
            width: parent.width
            spacing: 20
            visible: root.isLoggedIn
            
            Rectangle {
                id: syncreadyBtn
                width: parent.width; height: 50; color: "#0078D7"; radius: 5
                Text { text: "Синхронизировать"; color: "white"; font.pixelSize: 18; anchors.centerIn: parent }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.startSyncOnly()
                }
            }
            
            // НОВАЯ КНОПКА ВЫХОДА
            Rectangle {
                width: parent.width; height: 50; color: "#d9534f"; radius: 5
                Text { text: "Выйти из аккаунта"; color: "white"; font.pixelSize: 18; anchors.centerIn: parent }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.logout()
                }
            }
        }

        Text {
            id: statusText
            text: ""
            color: "white"
            font.pixelSize: 14
            wrapMode: Text.WordWrap
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Connections {
        target: syncManager
        onProgressUpdated: statusText.text = message
        onAuthCodeReceived: { authCodeContainer.visible = true; authCodeText.text = userCode; }
        onSyncFinished: { statusText.text = message; }
        // РЕАГИРУЕМ НА ВЫХОД
        onAuthStatusChanged: { root.isLoggedIn = isLoggedIn; syncManager.startAuthAndSync(clientIdInput.text, clientSecretInput.text) }
    }
}
