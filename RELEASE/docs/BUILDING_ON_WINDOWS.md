# Building on Windows

How the setup tool builds Call of Duty 3 on a Windows PC, what it installs (always after you approve the list, and where), and what to do when a build stops. The short version: extract the download to a short folder outside OneDrive, double-click `setup.bat`, approve the lists, and press **Build**.

## What you need

| | |
|---|---|
| **Windows** | 64-bit Windows 10 (22H2) or 11 on an x86-64 (Intel or AMD) PC. Windows on ARM runs the tools and the game through its x64 emulation; that hasn't been tested. |
| **Disk** | About 30 GB free: 8 GB in the work folder while building (the SDK and the compiled game), about 12 GB for the output folder (the game, its files from your disc, the rips), up to 3 GB for the downloaded tools in `work\tools`, and 3-6 GB on drive C: for Microsoft's Visual Studio Build Tools. |
| **Memory** | 8 GB or more. Each build thread takes about 2 GB; the tool picks the number for you. |
| **Rights** | A normal user account. Windows asks for administrator approval (UAC) once, when Microsoft's Build Tools install. |
| **Internet** | For the downloads below (a few GB the first time). Everything is kept for the next build. |
| **Your discs** | The Call of Duty 3 Xbox 360 disc image (`.iso`). World at War is optional. |

**Where to put the project folder.** Extract the whole zip first (right-click, **Extract All**): starting `setup.bat` from inside the zip in File Explorer copies only that one file. Then put the folder at a short, plain path that OneDrive doesn't sync, for example `C:\Games\COD3`.

- **OneDrive** (Desktop, Documents and Pictures are often synced) uploads every file the build writes and locks files while it does, which makes builds fail with "Access is denied". "Check this PC" warns about it.
- **Spaces, long paths and letters like é or ж** in the folder's path are handled: the build reaches the work folder through a short link (`C:\c3w-work` or `c3w-work` in your `AppData\Local`, a directory junction that needs no admin rights), and Git is told to allow long paths. The files stay in the project folder.

## The one-click path

1. **`setup.bat`** looks for a 64-bit Python 3.9 or newer with Tkinter (the window): first its own in `work\tools\python`, then the `py` launcher, then `python` on PATH. Without one it offers to install Python 3.12 with winget into `work\tools\python` and waits for your **y**. (No winget, as on some Windows 10 or LTSC installs: install the 64-bit Python from [python.org](https://www.python.org/downloads/windows/) and tick "tcl/tk and IDLE".)
2. The setup tool's window opens. Pick your disc image and press **Build**.
3. **The approval list** shows everything the build will download or install, where each one comes from and the folder it goes into. Nothing is downloaded before you approve. If you say no, nothing changes.
4. **Tools**: the downloads unpack into `work\tools`; Visual Studio's Build Tools install with Microsoft's installer (UAC prompt, a progress window, 10-30 minutes). The log shows what was found, with versions and paths.
5. **The SDK** ([ReXGlue](https://github.com/rexglue/rexglue-sdk)) is fetched with Git at a pinned commit with its submodules (about 1.5 GB), patched (`COD_3_WAW_Setup-tool\patches`) and built with clang in Visual Studio's build environment.
6. **The game's code** is translated from the disc's PowerPC code to C++ and compiled: the long part, 20-90 minutes.
7. **Packaging**: `bin\cod3.exe` with every DLL it needs (the Visual C++ runtime included), a shortcut and `Launcher.bat` in the output folder.

A build that stops or is canceled continues where it stopped when you press **Build** again.

## What each approval downloads, and where it goes

Each tool is a pinned release, checked against its SHA-256 before use (`c3w/deps.py`; sources in [SOURCES.md](SOURCES.md)). Downloads are kept in `work\tools\downloads`; `work\tools` survives the cleanup, so the next build needs none of them again.

| What | Only when | From | Goes into |
|---|---|---|---|
| Python 3.12 (asked by `setup.bat`) | no usable Python | winget (python.org's installer) | `work\tools\python` |
| 7-Zip 24.09 (`7zr.exe`, then the full 7-Zip unpacked from its installer) | no 7-Zip | github.com/ip7z | `work\tools\7zr-win`, `work\tools\7zip-win` |
| MinGit 2.48.1 (Git for Windows' portable version) | no Git, or one older than 2.31 | github.com/git-for-windows | `work\tools\git-win` |
| LLVM/clang 20.1.8 (unpacked with 7-Zip; its installer isn't run) | no clang 19+ for x86-64 MSVC | github.com/llvm | `work\tools\llvm-win` (2.5 GB) |
| CMake 3.31.6, Ninja 1.12.1 | missing or too old | github.com | `work\tools\cmake-win`, `work\tools\ninja-win` |
| innoextract 1.9 (optional: map pack installers) | not installed | github.com | `work\tools\innoextract-win` |
| Visual Studio 2022 Build Tools: C++ x64 tools and the Windows 11 SDK | no Visual Studio with C++, or no Windows SDK | Microsoft (winget, or Microsoft's own installer after its signature is checked) | `Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` and `Windows Kits` (the only things outside the project folder) |
| Rust (rustup, minimal) | no prebuilt ripper (a release download has `engine\bin\win-amd64\rip.exe`, then no Rust is needed) | static.rust-lang.org | `work\tools\rust` |
| The ripper's Rust libraries | Rust builds the ripper | crates.io | `work\tools\rust\cargo\registry` |
| ReXGlue SDK source with its submodules | first build | github.com | `work\rexglue-sdk` |
| AMD FidelityFX SDK source (FSR / CAS in the game) | "AMD FidelityFX" is on (Set up page, "Show more options") | github.com, fetched by the SDK's own CMake setup | `work\rexglue-sdk\out\build\win-amd64\_deps` |
| waifu2x-ncnn-vulkan (optional texture upscaling) | the Textures option is on | github.com/nihui | `work\tools\waifu2x-win` |
| Ubuntu 24.04 for WSL | only when the Linux version is built too | releases.ubuntu.com | `work\wsl`, removed after the build |

Rust: when Visual Studio's C++ tools are there, Rust uses Microsoft's linker (the `x86_64-pc-windows-msvc` toolchain); without them it gets the GNU toolchain, which brings its own. A Rust you installed yourself is used as it is.

## Check this PC

**Tools** page, **Check this PC** (or `setup.bat doctor` in a Command Prompt) lists every tool with its version and folder, and under it, for each missing or wrong piece, **why** the build needs it and the exact **fix**: the button that installs it with your approval, or what to install yourself and where to get it. It also warns about what on this PC tends to break Windows builds: a folder inside OneDrive, the Microsoft Store's Python, a 32-bit Python, Windows on ARM, a proxy that Git doesn't know about, little free space. **Copy** puts the whole result on the clipboard.

Every build's log starts with the setup tool's and Python's versions and Windows' version, then the tools it found (with versions and paths) and the same notes. When a command fails and the cause is known, the error message ends with **What to do:**. To report a problem, use **Log**, **Make bug report...**: it zips the newest logs with user, computer and folder names removed.

## When a build stops

| You see | Why | What to do |
|---|---|---|
| "Windows protected your PC" when starting `setup.bat` | SmartScreen: the file came from the internet | **More info**, **Run anyway**. Or before extracting: right-click the zip, **Properties**, tick **Unblock**. |
| "The setup tool's files aren't next to this setup.bat" | `setup.bat` was started from inside the zip | Extract the whole zip first (right-click, **Extract All**). |
| "Python was not found; run without arguments to install from the Microsoft Store" | Windows' placeholder for Python | Run `setup.bat`, which installs one, or install Python from python.org. |
| "The window needs Tkinter" | That Python was installed without "tcl/tk and IDLE" | Run `setup.bat`, or Settings, Apps, Python, **Modify**, tick "tcl/tk and IDLE". |
| "Visual Studio installer: exit code 1602" | The UAC prompt or the installer was closed | Press **Build** again and approve the prompt. |
| "exit code 3010" | Installed; Windows wants a restart | Restart Windows, then **Build**. |
| "Visual Studio is installed, but its build environment didn't start" | `vcvars64.bat` failed (a damaged installation) | Visual Studio Installer, **Repair** (or **Modify**, "Desktop development with C++"). A very long PATH ("The input line is too long") is handled by itself. |
| "The Windows SDK" missing, `'windows.h' file not found`, `kernel32.lib`, `rc.exe` | The Build Tools lack the Windows SDK or the x64 tools | **Install missing**, or in the Visual Studio Installer: **Modify**, tick "MSVC v143 x64/x86 build tools" and "Windows 11 SDK". |
| `STL1000: Unexpected compiler version` | An older LLVM on PATH is too old for Visual Studio's C++ library | Remove the old LLVM from PATH (or uninstall it); the tool then uses its own LLVM 20. |
| `linker 'link.exe' not found` (the ripper) | Your Rust uses Microsoft's linker and the Build Tools aren't there | Build with the playable game on (it installs them), or `rustup default stable-x86_64-pc-windows-gnu`. |
| `Filename too long` | A path went over Windows' 260 characters | Move the project folder to a short path like `C:\Games\COD3`, or turn on long paths (Windows 11: Settings, System, For developers, **Enable long paths**). |
| `SSL certificate problem` | An antivirus's web shield or a company proxy intercepts HTTPS | `git config --global http.sslBackend schannel` in a Command Prompt (Git then uses Windows' certificates), or switch the HTTPS scanning off while building. |
| `Could not resolve host`, `Failed to connect`, `early EOF` | No connection, or a proxy | Check the connection and build again (it continues). Behind a proxy: `set HTTPS_PROXY=http://<proxy>:<port>` in a Command Prompt, then start `setup.bat` from that window. |
| `Access is denied`, `being used by another process`, `LNK1104` | A file the build had to change was open | Close the game and any File Explorer window in the work folder; move the project out of OneDrive; if your antivirus keeps doing it, add the project folder to its exclusions. Then **Build**. |
| `Access is denied` with a "Unauthorized changes blocked" notice | Windows Security's Controlled folder access guards Documents, Desktop, Pictures, Videos and Music | Move the project folder to a folder like `C:\Games\COD3`, or allow the programs (Windows Security, Virus & threat protection, Ransomware protection). "Check this PC" warns about it when Windows lets it read the setting. |
| "Call of Duty 3 is running" | Windows can't replace a running program | Close the game, then **Build**. |
| `Patch ... doesn't apply` | The SDK checkout was changed by hand or half-done | Delete `work\rexglue-sdk` and build again. (Patches whose line endings were changed to CRLF are converted back by the tool.) |
| A long pause at "Configuring the SDK (FidelityFX on)" | CMake is downloading AMD's FidelityFX SDK (no progress shown) | Wait; or untick "AMD FidelityFX" under "Show more options". If it fails, the SDK is built again without it. |
| Out of memory, `C1060`, the PC freezes | Too many build threads for the memory | Lower "Build threads" (Set up page, "Show more options"). |
| Builds are very slow | Antivirus real-time scanning of every compiled file | Optional: add the project folder to Windows Security's exclusions (Virus & threat protection, Manage settings, Exclusions). Your choice; the build works without it. |

**PowerShell's execution policy** doesn't matter: the setup tool runs no `.ps1` scripts. It asks PowerShell only two short questions (whether Microsoft signed its installer, and to make the game's shortcut), which the policy doesn't cover.

**Line endings.** `.bat` files must keep Windows line endings (CRLF) and the patches Linux ones (LF). Git takes care of that through `.gitattributes`; a zip from the release page has them right too.

## Known limits

- The Windows version built on Windows (Visual Studio's libraries with clang) is the SDK's own route, but this project has mostly been built for Windows on Linux with llvm-mingw (`sh setup.sh` with the Windows version ticked). If a Windows build stops in the compile, please send the bug report.
- Under Wine the rips work, but Visual Studio's Build Tools don't install, so the game isn't compiled there.
