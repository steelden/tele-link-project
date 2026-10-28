# Build instructions for Windows

- [Prepare folder](#prepare-folder)
- [Install third party software](#install-third-party-software)
- [Initialize terminal](#initialize-terminal)
- [Clone source code and prepare libraries](#clone-source-code-and-prepare-libraries)
- [Configure the project](#configure-the-project)
- [Build the project](#build-the-project)
- [Notes](#notes)
- [Qt Visual Studio Tools](#qt-visual-studio-tools)

## Prepare folder

TeleLink is built for Windows 64-bit in **Visual Studio 2026** with **10.0.26100.0** SDK version.

Choose an empty folder for the future build, for example **D:\\TBuild**. It will be named ***BuildPath*** in the rest of this document. Create two folders there, ***BuildPath*\\ThirdParty** and ***BuildPath*\\Libraries**.

The default modern toolset from Visual Studio 2026 (`v145`) does not support Windows 7, so you must use `-vcvars_ver=14.44` (`v144.4`, based on `v143` with Windows 7 support).

## Install third party software

* Download **Python 3.10** installer from [https://www.python.org/downloads/](https://www.python.org/downloads/) and install it with adding to PATH.
* Download **Git** installer from [https://git-scm.com/download/win](https://git-scm.com/download/win) and install it.

## Initialize terminal

Before preparing libraries and running build commands, initialize the Visual Studio environment:

    %comspec% /k "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44

Run the next sections in the terminal initialized with this command.

## Clone source code and prepare libraries

In the initialized terminal, go to ***BuildPath*** and run

    git clone --recursive https://github.com/steelden/tdesktoplink.git tdesktop
    tdesktop\Telegram\build\prepare\win.bat

Preparing the libraries takes a long time, it is done once.

## Configure the project

TeleLink works with the MTS Link servers only and does not use the Telegram API, but the configuration still requires **api_id** and **api_hash**: use the test ones from [api_credentials.md](api_credentials.md).

Go to ***BuildPath*\\tdesktop\\Telegram** and run

    configure.bat x64 -D TDESKTOP_API_ID=17349 -D TDESKTOP_API_HASH=TEST_API_HASH

with **TEST_API_HASH** replaced by the test **api_hash**. The project is generated in ***BuildPath*\\tdesktop\\out**.

## Build the project

Close the running TeleLink first (`taskkill /F /IM TeleLink.exe`), otherwise its executable can't be replaced. In ***BuildPath*\\tdesktop** run

    cmake --build out --config Debug --target Telegram

for the debug version, or

    cmake --build out --config Release --target Telegram --target Updater

for the release one. The result **TeleLink.exe** is located in ***BuildPath*\\tdesktop\\out\\Debug** (and **Release**). The log of the current run is **log.txt** next to it, the logs of the previous runs are in the **DebugLogs** folder.

You can also open ***BuildPath*\\tdesktop\\out\\Telegram.slnx** in Visual Studio 2026, select the Telegram project and press Build > Build Telegram.

## Notes

* The compiler processes of a project are limited by the memory (4 GB each, up to the number of the logical processors): too many of them with the precompiled headers fail with **C1076** or **C3859**. If it happens, set fewer of them, for example `cmake out -D TELELINK_COMPILE_PROCESSES=8`.
* After adding the keys to **Telegram\\Resources\\langs\\lang.strings** touch **Telegram\\SourceFiles\\lang\\lang_instance.cpp** and **lang_file_parser.cpp** before the build: they are not rebuilt by themselves, and TeleLink fails on the start with "vector subscript out of range".
* The Russian texts are in **Telegram\\Resources\\langs\\ru.strings**.

## Qt Visual Studio Tools

For better debugging you may want to install Qt Visual Studio Tools:

* Open **Extensions** -> **Manage Extensions**
* Go to **Online** tab
* Search for **Qt**
* Install **Qt Visual Studio Tools** extension
