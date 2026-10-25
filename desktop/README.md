# Sreon for Linux and Windows

The Mac app has a native Swift shell. This folder is the same browser with a shell for
Linux and Windows, built on [Electron](https://www.electronjs.org). Nothing about the
browser itself is different: every page, setting and decision is still the SPRFST engine
(`../engine`), and the interface is still the same HTML, CSS and JavaScript (`../ui`).
This shell only supplies windows, tabs and web views, and talks to the engine over the
same JSON-lines protocol the Mac shell uses.

```
main.js           Electron: windows, tabs, the sreon:// scheme, downloads, Shield, menu
preload.js        gives Sreon's pages the same bridge the Mac app has (no UI code changed)
src/engine.js     starts the SPRFST engine and talks to it
src/controller.js one window: tabs, workspaces, split view, session, every engine action
src/layout.js     where the sidebar, toolbar, panel and page go
src/rules.js      turns Shield's block lists into a request filter
src/shell.js      settings, theme, scripts and rules shared by all windows
test/             the logic above, tested against the real engine
```

## Build a Linux .deb (amd64 or arm64)

You need `gcc`, `make`, Node 18+ and an internet connection (the first time, npm downloads
Electron). One command per architecture:

```bash
cd Sreon
make deb              # the architecture of the machine you are on
make deb-arm64        # 64-bit ARM: Raspberry Pi 4/5, Ampere, Apple-silicon Linux VMs, Chromebooks
make deb-amd64        # 64-bit Intel/AMD
make deb-all          # both
```

Results land in `desktop/dist/`: `sreon_1.1.0_amd64.deb` and `sreon_1.1.0_arm64.deb`.
Install on a machine of the same architecture:

```bash
sudo apt install ./desktop/dist/sreon_1.1.0_arm64.deb
sreon
```

The script builds the SPRFST compiler from source for the target, gathers the engine and its
files, runs the tests, and packages everything.

### On a Mac

A Mac cannot build a Linux package directly (it would compile Mac programs). `make deb-arm64`
notices this and does the build inside a Linux container with Docker instead:

```bash
brew install --cask docker           # or install Docker Desktop, then open it once
make deb-arm64                       # on an Apple-silicon Mac this is native and quick
make deb-amd64                       # works too, but is emulated and slow
```

The package appears in `desktop/dist/`. Copy it to the Linux machine and install it there with
`sudo apt install ./sreon_1.1.0_arm64.deb`. Docker must be running, and the first build
downloads a few hundred MB.

### Building arm64

**On an arm64 machine** (easiest, and the packages are tested there):

```bash
sudo apt install build-essential nodejs npm ruby ruby-dev
sudo gem install fpm                 # electron-builder's packager only exists for x86
make deb-arm64
```

**On an x86 machine** (cross-compiling):

```bash
sudo apt install build-essential nodejs npm gcc-aarch64-linux-gnu
make deb-arm64
```

The compiler is built with `aarch64-linux-gnu-gcc`, and electron-builder downloads the arm64
build of Electron. Build on the oldest Ubuntu/Debian you want to support, because the
program links against that system's glibc.

An arm64 binary cannot run on an x86 machine, so a cross build skips the tests; the engine files
are still checked with the local compiler. After installing on the real device, run
`sreon` once and confirm the boot splash, the new tab and Studio's Run button.

What has been checked for arm64 without an arm64 machine: the compiler builds and passes its whole
suite with `-funsigned-char`, which reproduces the one behaviour that differs from x86
(`char` is unsigned on arm64 Linux), and `sys.arch()` already reports `arm64`. What has not:
an actual arm64 build or run.

To run without packaging:

```bash
make && cd desktop && npm install && npm start
```

## Windows

**Windows 11 today: run the Linux package in WSL2.** WSL2 shows Linux apps as normal
windows (WSLg), and this works with no changes:

```powershell
wsl --install -d Ubuntu          # once, then reboot
```
```bash
# inside Ubuntu, after building the .deb as above
sudo apt install ./desktop/dist/sreon_1.1.0_amd64.deb
sreon
```

**A native Windows installer is not finished.** The shell and the installer settings
(`"win"` in `package.json`) are ready, but the SPRFST compiler is written for POSIX and
has no Windows build yet. Before `electron-builder --win` produces something that runs:

1. `compiler/src` needs porting to Windows: sockets (`sys/socket.h`, `netdb.h`,
   `arpa/inet.h`) to Winsock, plus `dirent`, `pthread` and `unistd` through MinGW-w64
   or MSVC shims.
2. Studio and Tools run small shell commands (`sh`, `perl`, `uuidgen`, `head`, `base64`)
   through `sys.run`. They need a POSIX shell on the PATH (MSYS2, Git Bash) or to be
   rewritten.

Neither has been done or tested.

## What is and is not the same as the Mac app

Same: tabs, pinning, sleeping tabs, workspaces, split view, the sidebar (expanded, compact
or hidden), the command bar, the right-hand panel, Reader and Brief, Studio with live app
previews, the guidebook, Shield, history, downloads, per-site zoom, screenshot and PDF
(saved to Downloads), find in page, private windows, session restore, the boot splash.

Different:

- **No frosted glass.** The sidebar and panel are solid theme colours.
- **Shield** blocks requests and hides page furniture, but its script runs only in the
  main page, not in embedded frames.
- **Shortcuts** use Ctrl where the Mac uses Cmd.
- **Menu bar** is hidden (press Alt to show it); every command is also in the command bar.
- Downloads show a toast when finished; there is no live progress.

## Honest status

Verified here, with the real engine: the engine connection, the window logic (tabs,
sidebar hide and show, dock buttons, workspaces, split, session save and restore, private
windows, page events, history), window layout, the Shield rule filter, shortcut matching,
that the staged package runs on its own, and a start-up run of the real `main.js` against a stand-in for Electron (splash first, window after, bridge locked to Sreon pages, Shield blocks trackers, closing the last window quits). That is 15 logic tests, the start-up run, and a shortcut check.

Not verified: `main.js` against real Electron. The start-up run above uses a stand-in
for Electron, so it proves the code's own logic and order, not that Electron behaves
as assumed. Expect a first-run bug or two. The Electron version is pinned
(`33.2.1`) because the code uses `WebContentsView`, `BaseWindow` and
`webContents.navigationHistory`, which need Electron 32 or newer.
