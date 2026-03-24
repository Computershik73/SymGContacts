import QtQuick 1.0

Rectangle {
    id: root
    width: 360
    height: 640
    color: "#001e36"

    property bool isPortrait: root.width < root.height
    property bool isLoggedIn: syncManager.hasToken()

    // ================================================================
    // FILE PICKER STATE
    // ================================================================
    property bool showFilePicker: false
    property variant jsonFiles: []

    // When filePicker emits credentialsLoaded — fill in the fields
    Connections {
        target: filePicker
        onCredentialsLoaded: {
            clientIdInput.text     = clientId
            clientSecretInput.text = clientSecret
            root.showFilePicker    = false
            statusText.text        = "Credentials loaded from file"
        }
        onLoadError: {
            root.showFilePicker = false
            statusText.text     = "Error: " + message
        }
    }

    // ================================================================
    // MAIN COLUMN
    // ================================================================
    Column {
        anchors.centerIn: parent
        spacing: root.isPortrait ? 16 : 8
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
            font.pixelSize: 14
            color: "#aaaaaa"
            anchors.horizontalCenter: parent.horizontalCenter
        }

        // ---- LOGIN BLOCK ----
        Column {
            width: parent.width
            spacing: 12
            visible: !root.isLoggedIn && !root.showFilePicker

            // Load from file button
            Rectangle {
                width: parent.width; height: 50
                color: "#28a745"; radius: 5
                Text {
                    text: "Load client_secret.json"
                    color: "white"; font.pixelSize: 16
                    anchors.centerIn: parent
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.jsonFiles      = filePicker.findJsonFiles()
                        root.showFilePicker = true
                    }
                }
            }

            // Divider
            Text {
                text: "— or enter manually —"
                color: "#888888"; font.pixelSize: 13
                anchors.horizontalCenter: parent.horizontalCenter
            }

            // Client ID
            Text { text: "Client ID:"; color: "#aaaaaa"; font.pixelSize: 13 }
            Rectangle {
                width: parent.width; height: 40; color: "white"; radius: 5
                TextInput {
                    id: clientIdInput
                    anchors.fill: parent; anchors.margins: 5
                    font.pixelSize: 14; color: "black"
                }
            }

            // Client Secret
            Text { text: "Client Secret:"; color: "#aaaaaa"; font.pixelSize: 13 }
            Rectangle {
                width: parent.width; height: 40; color: "white"; radius: 5
                TextInput {
                    id: clientSecretInput
                    anchors.fill: parent; anchors.margins: 5
                    font.pixelSize: 14; color: "black"
                    echoMode: TextInput.Password
                }
            }

            // Sign in button
            Rectangle {
                width: parent.width; height: 50; color: "#0078D7"; radius: 5
                Text {
                    text: "Sign in and Sync"
                    color: "white"; font.pixelSize: 18
                    anchors.centerIn: parent
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.startAuthAndSync(
                        clientIdInput.text, clientSecretInput.text)
                }
            }

            // Auth code box
            Rectangle {
                id: authCodeContainer
                width: parent.width; height: 100
                color: "#333333"; radius: 5
                visible: false
                Column {
                    anchors.centerIn: parent
                    spacing: 4
                    Text { text: "Code for Google:"; color: "white"; font.pixelSize: 14 }
                    Text {
                        id: authCodeText
                        text: "---"; color: "#ff9900"
                        font.pixelSize: 24; font.bold: true
                    }
                    Text {
                        text: "Enter at google.com/device"
                        color: "#aaaaaa"; font.pixelSize: 12
                    }
                }
            }
        }

        // ---- FILE PICKER BLOCK ----
        Column {
            width: parent.width
            spacing: 8
            visible: root.showFilePicker

            Text {
                text: "Select JSON file:"
                color: "white"; font.pixelSize: 16; font.bold: true
            }

            // File list
            Repeater {
                model: root.jsonFiles
                delegate: Rectangle {
                    width: parent.width; height: 44
                    color: fileMouseArea.pressed ? "#0078D7" : "#1a3a5c"
                    radius: 4
                    Text {
                        text: modelData.split("/").pop()
                        color: "white"; font.pixelSize: 14
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideLeft
                        width: parent.width - 20
                    }
                    MouseArea {
                        id: fileMouseArea
                        anchors.fill: parent
                        onClicked: filePicker.loadFile(modelData)
                    }
                }
            }

            // No files found message
            Text {
                visible: root.jsonFiles.length === 0
                text: "No .json files found on E:\\ or C:\\Data\nCopy client_secret.json to E:\\ and try again"
                color: "#ff9900"; font.pixelSize: 13
                wrapMode: Text.Wrap
                width: parent.width
            }

            // Cancel button
            Rectangle {
                width: parent.width; height: 44; color: "#555555"; radius: 5
                Text {
                    text: "Cancel"
                    color: "white"; font.pixelSize: 16
                    anchors.centerIn: parent
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.showFilePicker = false
                }
            }
        }

        // ---- LOGGED IN BLOCK ----
        Column {
            width: parent.width
            spacing: 16
            visible: root.isLoggedIn

            Rectangle {
                width: parent.width; height: 50; color: "#0078D7"; radius: 5
                Text {
                    text: "Synchronize"
                    color: "white"; font.pixelSize: 18
                    anchors.centerIn: parent
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.startSyncOnly()
                }
            }

            Rectangle {
                width: parent.width; height: 50; color: "#d9534f"; radius: 5
                Text {
                    text: "Sign out"
                    color: "white"; font.pixelSize: 18
                    anchors.centerIn: parent
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: syncManager.logout()
                }
            }
        }

        // ---- STATUS TEXT ----
        Text {
            id: statusText
            text: ""
            color: "#aaaaaa"; font.pixelSize: 13
            wrapMode: Text.Wrap
            width: parent.width
            anchors.horizontalCenter: parent.horizontalCenter
        }
    }

    // ================================================================
    // CONNECTIONS TO SYNCMANAGER
    // ================================================================
    Connections {
        target: syncManager
        onProgressUpdated: { statusText.text = message }
        onAuthCodeReceived: {
            authCodeContainer.visible = true
            authCodeText.text         = userCode
            statusText.text           = verificationUrl
        }
        onSyncFinished: {
            statusText.text        = message
            root.isLoggedIn        = syncManager.hasToken()
            authCodeContainer.visible = false
        }
        onAuthStatusChanged: {
            root.isLoggedIn = isLoggedIn
        }
    }
}
