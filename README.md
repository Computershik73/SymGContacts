# SymGContacts

[Readme на русском](https://github.com/Computershik73/SymGContacts/blob/main/README.ru.md)

**SymGContacts** is a full-featured client for two-way synchronization of Google Contacts for devices running Symbian OS (S60 3rd Edition Feature Pack 1, 5th Edition, Symbian^3 / Anna / Belle).

The project was developed in 2026 and proves that Symbian can still work with modern cloud services. The application uses the up-to-date **Google People API (+OAuth 2.0)** and directly interacts with the Symbian system contact database (`cntmodel.lib`), ensuring reliable operation without relying on outdated or discontinued services (such as Mail for Exchange or SyncML).

## Key Features

*   **Two-way "Smart" Sync:** Priority is always given to the most recent changes (whether edits were made on the phone or in a browser).
*   **Full field synchronization:** Supports names, multiple phone numbers, email addresses, postal addresses, company names, job titles, birthdays, and notes.
*   **Batch Upload:** High performance when adding new contacts thanks to batch requests to the Google API.
*   **Deduplication (Merge):** Automatically detects and merges duplicate local contacts before uploading to the cloud.
*   **Background operation:** The app can start with the system and quietly sync contacts on a timer (every 15 minutes) without interrupting device usage.
*   **Modern security:** Uses OAuth 2.0 (Out-Of-Band flow) and supports TLS 1.2.

---

## System Requirements

### To run the app on a device:
*   A smartphone running **Symbian 9.2 (S60 3rd Edition Feature Pack 1)** or newer.
*   **IMPORTANT:** To connect to Google servers (`people.googleapis.com`), the device **must have a patch installed that adds TLS 1.2 support** at the Qt library level. Without this patch, the app will fail during the SSL handshake and show a sync error. Download the patch here: https://nnproject.cc/qtls
*   Active internet connection (Wi-Fi recommended for the first sync of a large contact database).
*   *Required:* An unlocked smartphone (RomPatcher+ with the necessary patches installed) to allow installation of an unsigned `.sis` file.

### To build from source (Developer environment):
Building for Symbian requires setting up a legacy environment. It is recommended to use a virtual machine with Windows XP, Windows 7, or Windows 10.

*   [Qt SDK 1.1.2](https://nnm.nnchan.ru/dl/sdk/Qt_SDK_Win_offline_v1_1_2_en.exe) (includes Qt 4.7.3 for Symbian). 
*   **Nokia Symbian SDK** (included with the Qt SDK).
*   Compiler **GCCE** or **RVCT** (included with Qt SDK / Carbide.c++).

---

## Setting up a project in Google Cloud

Since the application uses the official API, you will need to create your own project in Google Cloud Console to obtain access credentials (Client ID and Client Secret).

1.  Go to [Google Cloud Console](https://console.cloud.google.com/).
2.  Create a new project (e.g., "Symbian Sync").
3.  In **"APIs & Services" -> "Library"**, find and enable **Google People API**.
4.  Go to **"OAuth consent screen"**:
    *   Choose type **"External"**.
    *   Fill in the required fields (application name, email).
    *   Under **"Test users"**, make sure to add your Google account (otherwise you won’t be able to sign in).
5.  Go to **"Credentials"**:
    *   Click "Create Credentials" -> "OAuth client ID".
    *   Select Application type: **"TVs and Limited Input devices"** (this is critical for the OOB authorization method used).
    *   Click "Create".
6.  Copy the **Client ID** and **Client Secret**. You will enter them in the application on first launch.

---

## Building the project

1.  Clone the repository:
    ```bash
    git clone https://github.com/Computershik73/SymGContacts.git
    cd SymGContacts
    ```
2.  Open the project `SymGContacts.pro` in **Qt Creator** (from the Nokia Qt SDK).
3.  Make sure the build profile is set to **Symbian Device** (Target: `Release`).
4.  Clean and regenerate the Makefile:
    *   `Build -> Clean Project`
    *   `Build -> Run qmake`
5.  Build the project (`Build -> Build Project`) and run it. Don’t forget to enable unsigned SIS package creation in the run settings.
6.  After a successful build, Qt Creator will generate a `.sis` file in the build directory.

---

## Architecture Notes (For Developers)

*   **No Qt Mobility:** The project intentionally does not use the `QContactManager` module (Qt Mobility) due to compatibility issues on devices like Samsung (Omnia HD) and Sony Ericsson (Satio/Vivaz). All contact handling is implemented in pure **Symbian C++ (`cntmodel.lib`)** via direct calls to `CContactDatabase`. TODO: In theory, this allows future backporting to older Symbian versions by removing dependency on Qt (which is mainly used here for the GUI).
*   **Exception isolation:** All calls to the unstable Symbian API are wrapped in `TRAPD` macros with proper use of `CleanupStack`, fully preventing OS kernel panics when reading or writing corrupted contacts.
*   **Deterministic hashing:** The application uses SHA-1 to determine whether a contact has changed (locally or in the cloud). The hash is built from sorted contact fields, preventing false positives when the order of phone numbers changes.
*   **State storage:** Synchronization data (`ETags` and local hashes) are safely stored in a simple text file `sync_state_v2.txt` in the application's `private` folder.

## 📝 License

This project is distributed under the MIT License. You are free to use, modify, and distribute this code.

*Developed with ❤️ for the Symbian enthusiast community in 2026.*
