# Ghostty Union

A native Qt6 Widgets cross-platform terminal emulator (**Linux**, **WSL2**, **macOS**, and **Windows PowerShell/ConPTY**) powered by **`libghostty-vt` (1.0+)**, featuring a **Solar Forge** cockpit UI and default terminal palette (with built-in **Ollie** theme switching), dynamic nested split trees with drag-and-drop rearrangement, native session & layout persistence, and a mounted **74px live system telemetry bar**.

> **Looking for WSL2, macOS & Windows Testers!**
> With the `v1.0.0` release, native **macOS** (POSIX PTY + Mach/BSD telemetry) and **Windows** (ConPTY + PowerShell 7/5.1 + Win32 telemetry) backends are now included alongside Linux/WSL2. We are actively looking for community testers on **WSL2 / WSLg**, **macOS** (Apple Silicon & Intel), and **Windows 10/11 (PowerShell)** to verify builds, HiDPI rendering, and platform edge cases. Please open an issue or PR with your OS version, Qt6 version, and any build or runtime logs!

## Features

### Terminal Engine (`libghostty-vt 1.0+`)
- **Full VT & protocol support:** Truecolor (24-bit RGB), built-in **Solar Forge** (default) and Ghostty **Ollie** palettes (switchable live via right-click **Terminal Theme** menu, persisted in settings, or overridden via `--theme solar-forge|ollie`), Kitty Keyboard Protocol & `modifyOtherKeys`, SGR mouse tracking, focus events, bracketed paste, synchronized output holds (mode `2026`), and double/dotted/dashed/curly underlines.
- **Inline graphics & Solar Forge image inspector (Kitty Graphics + iTerm2 `OSC 1337`):**
  - Full **Kitty Graphics Protocol** rendering (`320 MB` storage budget, direct `t=d`, local file `t=f`, temp file `t=t`, POSIX shared memory `t=s`, `a=f`/`a=a` animations, fixed/relative placements, and `U+10EEEE` virtual Unicode placeholders for TUI avatars/diagrams) plus **iTerm2 `OSC 1337 ; File=inline=1`** support across all Qt6 image formats (PNG, WebP, JPEG, AVIF, JPEG-XL, SVG, GIF, QOI).
  - **5-layer z-ordered compositing & native HiDPI:** Reports physical device-pixel cell dimensions (`TIOCSWINSZ` / `XTWINOPS`) for crisp 1:1 HiDPI rasterization, composites `BELOW_BACKGROUND` / cell BGs / `BELOW_TEXT` / foreground text / `ABOVE_TEXT` layers accurately, and strips `U+10EEEE` placeholder diacritics when copying plain-text selections.
  - **Interactive image UX:** Right-click any inline image for **Inspect Image…**, **Copy Image**, or **Save Image As…**, or `Ctrl+Hover` / `Ctrl+Click` to open the zoomable, pannable **Solar Forge Image Inspector** lightbox.
- **High-throughput rendering:** Per-row `QPixmap` cache keyed by `libghostty-vt` dirty state and selection range, 50,000-line / 50 MiB scrollback buffer, HiDPI-snapped font metrics (`FiraCode Nerd Font SemBd` with monospace fallback), and an animated interactive scrollbar gutter.
- **Shell integration & link handling:**
  - Auto-detects `xterm-ghostty` terminfo (`/usr/share/terminfo/x/xterm-ghostty`, falling back to `xterm-256color`) and Ghostty Zsh shell integration (`/usr/share/ghostty/shell-integration/zsh`) when installed.
  - `OSC 7` / PWD tracking for inheriting working directories in new tabs and splits.
  - `OSC 8` hyperlinks plus plain-text URL and `file:line:col` detection on `Ctrl+Hover` / `Ctrl+Click` (opens `file:line:col` in `kate` if present, otherwise `xdg-open` via `QDesktopServices`).
  - `OSC 52` clipboard writes and `OSC 9` / `OSC 777` desktop notifications via `org.freedesktop.Notifications`.
- **Leaked TUI mode recovery:**
  - Automatically sanitizes leaked input/display modes (Kitty keyboard stack, `modifyOtherKeys`, alternate screen, mouse tracking, focus reporting, synchronized output hold, hidden cursor) immediately after `OSC 133;D` (`GHOSTTY_SEMANTIC_PROMPT_COMMAND_END`) reaches VT ground — recovering cleanly from abruptly killed remote SSH TUIs without disturbing prompt modes or Powerlevel10k `OSC 133;A` redraws.
  - Non-destructive manual soft reset (`Ctrl+Shift+K` or right-click **Reset Input Modes**) cancels unfinished escape sequences and restores normal input without clearing scrollback.

### Mounted System Telemetry Bar (`74px`)
- **Live oscilloscope cards:** CPU load + max package temperature, RAM used (GiB & %), dynamically enumerated 1-to-N GPU cards (load, temperature, power, plus NVIDIA hotplug detection via `/proc/driver/nvidia/gpus`), network throughput + private/public IPv4 (click top row to copy private IP, bottom row to copy public IP), VPN tunnel beacon (click to copy tunnel IP), and a 1-second chronometer.
- **Power-aware:** Automatically unsubscribes from `org.kde.ksystemstats1` sensor updates whenever the window is minimized or hidden.

### Tabs & Dynamic Splits
- Nested horizontal and vertical split trees per tab with directional focus navigation, resizing, split zoom, orientation rotation, equalization, and MRU focus restoration when panes close.
- Live drag-and-drop pane rearrangement: swap panes, dock to any edge or root edge, or detach a split into its own tab.
- Custom Solar Forge modals for tab renaming, tab close confirmation, and window session save prompts, plus adaptive tab bar sizing.
- **Native session & layout persistence:** Save tabs, custom tab titles, nested split trees (orientations & ratios), pane working directories (`OSC 7` + Linux `/proc/<pid>/cwd`), and window geometry to `~/.config/ghostty-union/session.json`. Prompts on manual window close (with single-window **Save & Close** or multi-window **Save All (N)** array support), or save anytime via `Ctrl+Shift+S` / tab bar context menu. Restores automatically on launch unless `-d` or `--no-restore` is passed.

---

## Keyboard Shortcuts

### Window & Tabs
| Shortcut | Action |
|---|---|
| `Ctrl+Shift+N` | New window (inherits active working directory) |
| `Ctrl+Shift+S` | Save current window(s), tabs, splits, and working directories to session layout |
| `Ctrl+Shift+T` | New tab |
| `Ctrl+Shift+A` | Rename current tab |
| `Ctrl+Shift+W` | Close active split (or prompt to close tab if last split) |
| `Ctrl+Tab` / `Ctrl+PageDown` | Next tab |
| `Ctrl+Shift+Tab` / `Ctrl+PageUp` | Previous tab |
| `Alt+1` .. `Alt+8` | Switch to tab 1..8 |
| `Alt+9` | Switch to last tab |

### Splits
| Shortcut | Action |
|---|---|
| `Ctrl+Shift+O` / `Ctrl+Shift+}` / `Ctrl+Shift+]` | Split right (horizontal) |
| `Ctrl+Shift+E` / `Ctrl+Shift+{` / `Ctrl+Shift+[` | Split down (vertical) |
| `Ctrl+Alt+Arrow` | Focus neighbor split |
| `Alt+Shift+Arrow` | Resize active split |
| `Ctrl+Alt+Shift+Arrow` | Move / swap split directionally |
| `Ctrl+Shift+R` | Rotate split orientation |
| `Ctrl+Alt+Shift+E` | Equalize split sizes |
| `Ctrl+Shift+Enter` | Toggle split zoom |

### Terminal
| Shortcut | Action |
|---|---|
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy selection / Paste clipboard |
| `Ctrl+Click` (on link or image) | Open hyperlink/`file:line:col`, or open Solar Forge Image Inspector |
| `Ctrl+Shift+K` | Soft reset terminal input & display modes (keeps scrollback) |
| `Ctrl+Shift+Up` / `Ctrl+Shift+Down` | Jump to previous / next semantic prompt (`OSC 133`) |
| `Shift+PageUp` / `Shift+PageDown` | Scroll half-page up / down |
| `Ctrl++` / `Ctrl+=` / `Ctrl+Shift++` | Zoom font in |
| `Ctrl+-` | Zoom font out |
| `Ctrl+0` | Reset font size |
| `Ctrl+Backspace` | Erase previous word (`0x17`) |

---

## Platform & Desktop Environment Support

**Short answer:** `ghostty-union` runs natively on **Linux** (any modern distro and X11/Wayland DE/WM, plus **WSL2/WSLg**), **macOS** (Apple Silicon & Intel), and **Windows 10/11** (native **ConPTY** with PowerShell 7 `pwsh.exe` / Windows PowerShell `powershell.exe` / `cmd.exe`). It is **not** locked to Arch Linux or KDE Plasma.

### What Is Required vs. Optional

| Component | Scope | Notes |
|---|---|---|
| **Terminal Backend (PTY)** | Built-in per OS | **Linux:** POSIX `<pty.h>` + `/proc/<pid>/cwd`. **macOS:** POSIX `<util.h>` + `libproc` (`proc_pidinfo`). **Windows 10/11:** Native **ConPTY** (`CreatePseudoConsole`) launching `pwsh.exe` → `powershell.exe` → `%COMSPEC%`. |
| **Qt 6.8+** (`Core`, `Gui`, `Widgets`, `Network`, `Test`, + `DBus` on Linux) | Required | Pure Qt6 Widgets app — no dependency on KWin, Plasma Shell, `KXmlGui`, or `KParts`. Older LTS Linux distros (e.g., Ubuntu 24.04 with Qt 6.4) need a newer Qt6 toolchain. |
| **`libghostty-vt` >= 1.0** | Required | Packaged directly on Arch (`libghostty-vt`). On other Linux distros, macOS, and Windows, build from [`ghostty-org/ghostty`](https://github.com/ghostty-org/ghostty) via `zig build -Demit-lib-vt -Doptimize=ReleaseFast`. |
| **System Telemetry Bar** | Built-in per OS | **Linux:** Uses `libksysguard` (`ksystemstats` over D-Bus, optional at runtime). **macOS:** Native Mach/BSD (`host_statistics`, `sysctl`, `getifaddrs`) — no KDE dependencies. **Windows:** Native Win32 (`GetSystemTimes`, `GlobalMemoryStatusEx`, `GetIfTable2`) — no KDE dependencies. |
| **Ghostty terminfo & shell integration** | Optional | Auto-used if `/usr/share/terminfo/x/xterm-ghostty` and `/usr/share/ghostty/shell-integration/zsh` exist; otherwise cleanly falls back to `TERM=xterm-256color`. |
| **`kate`** | Optional | Used for `Ctrl+Click` on `file:line:col` links if found in `PATH`; otherwise falls back to `xdg-open` (`QDesktopServices`). |
| **FiraCode Nerd Font** | Optional | Preferred font (`FiraCode Nerd Font SemBd`); falls back to the system monospace font if not installed. |

### Current Machine-Specific Defaults to Keep in Mind
- **VPN telemetry card:** Currently monitors the network interface named `home-lan` (`src/StatsBarWidget.cpp`). On machines without a `home-lan` interface, the card safely shows `VPN off`.
- **Public IP lookup:** Periodically queries `api4.ipify.org` (with fallbacks to `ipv4.icanhazip.com` and `ifconfig.me/ip`) for the NET card's `PUB` readout.

---

## Building from Source

### Prerequisites

- CMake >= 3.24 and a C++20 compiler (GCC or Clang)
- Qt 6.8+ (`Core`, `Gui`, `Widgets`, `Network`, `DBus`, `Test`)
- `libksysguard` (development headers + `KSysGuard` CMake config), `libsensors` development files (`libsensors-dev` on Debian/Ubuntu, `lm_sensors-devel` on Fedora — required at link time by `KSysGuard::SystemStats`), and optionally `ksystemstats` at runtime
- `libghostty-vt` >= 1.0 (headers + `libghostty-vt.so.1`; discovered via `ghostty-vt-config.cmake` or `/usr[/local]/{include,lib}`)

#### Arch Linux (Native or WSL2)
```bash
sudo pacman -S --needed cmake base-devel qt6-base libksysguard ksystemstats libghostty-vt
```
*(Optional for shell integration and icons: `ghostty-shell-integration ghostty-terminfo ttf-firacode-nerd`. If you prefer to build `libghostty-vt` from source instead of using the repo package, omit `libghostty-vt` and follow the from-source steps below.)*

#### Fedora (41+, Native or WSL2)
```bash
sudo dnf install cmake gcc-c++ make git curl tar xz qt6-qtbase-devel libksysguard-devel lm_sensors-devel ksystemstats
```
Then build and install `libghostty-vt` from source using the official Zig tarball matching `ghostty`'s `build.zig.zon` (`minimum_zig_version`, currently `0.17.1`) as shown below.

#### Debian (13/Trixie+) / Ubuntu (25.04+, Native or WSL2)
```bash
sudo apt update
sudo apt install cmake build-essential git curl xz-utils qt6-base-dev libksysguard-dev libsensors-dev ksystemstats
```
- **`libsensors-dev` is required even in WSL2:** `libksysguard-dev`'s `KSysGuard::SystemStats` CMake target links against `libsensors.so` at build time, and Debian/Ubuntu do not always pull `libsensors-dev` in automatically with `libksysguard-dev`.
- **Do not use `apt install zig`:** Debian/Ubuntu repositories ship older Zig releases (e.g. `0.14.x`), which fail to compile `ghostty` (`main` currently requires Zig `0.17.1`, declared in `build.zig.zon`). Fetch the exact official Zig tarball from `ziglang.org` as shown in the next section.

> **Note for Ubuntu 24.04 LTS:** Ubuntu 24.04 ships Qt 6.4, which is below the `Qt6 6.8+` requirement in `CMakeLists.txt`. Use **Ubuntu 25.04+**, **Debian 13 (Trixie)+**, **Fedora 41+**, or **Arch Linux** (native or WSL2).

#### macOS (Apple Silicon & Intel)
```bash
brew install cmake qt@6
```
Build `libghostty-vt` from `ghostty-org/ghostty` using the official Zig tarball matching `build.zig.zon`:
```bash
zig build -Demit-lib-vt -Doptimize=ReleaseFast --prefix "$HOME/.local"
```
Then configure and build `ghostty-union` (no KDE / `libksysguard` required):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$HOME/.local"
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

#### Windows 10/11 (Native PowerShell / ConPTY)
Requires **Visual Studio 2022** (C++20 workload) or **MinGW-w64**, **CMake >= 3.24**, **Qt 6.8+** (installed via Qt Online Installer or `aqtinstall`), and **`libghostty-vt`** built from `ghostty-org/ghostty` with the official Zig release matching `build.zig.zon`:
```powershell
# In ghostty checkout (builds libghostty-vt into C:\ghostty-vt):
zig build -Demit-lib-vt -Doptimize=ReleaseFast --prefix C:\ghostty-vt

# In ghostty-union checkout (no KDE / libksysguard required):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64;C:\ghostty-vt"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```
On launch, `ghostty-union.exe` automatically spawns **PowerShell 7** (`pwsh.exe`) if installed, falling back to **Windows PowerShell** (`powershell.exe`) and then `%COMSPEC%` (`cmd.exe`) over native Windows **ConPTY**.

### Building `libghostty-vt` from Source

Needed on **Fedora**, **Debian**, and **Ubuntu** (and optional on **Arch** if not installing the `libghostty-vt` package). Works identically on native Linux and WSL2:

```bash
git clone https://github.com/ghostty-org/ghostty.git
cd ghostty

# Download the exact official Zig tarball matching build.zig.zon (currently 0.17.1).
# Avoid distro packages such as `apt install zig` (0.14.x), which cannot build ghostty.
ZIG_VERSION="$(sed -n 's/.*\.minimum_zig_version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' build.zig.zon | head -n 1)"
test -n "$ZIG_VERSION"
ARCH="$(uname -m)" # x86_64 or aarch64
rm -rf "/tmp/zig-${ZIG_VERSION}" && mkdir -p "/tmp/zig-${ZIG_VERSION}"
curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-${ARCH}-linux-${ZIG_VERSION}.tar.xz" \
  | tar -xJ --strip-components=1 -C "/tmp/zig-${ZIG_VERSION}"
export PATH="/tmp/zig-${ZIG_VERSION}:$PATH"
zig version

rm -rf /tmp/libghostty-vt-stage
zig build -Demit-lib-vt -Doptimize=ReleaseFast --prefix /tmp/libghostty-vt-stage
sudo cp -a /tmp/libghostty-vt-stage/. /usr/local/
sudo ldconfig
```

To install `libghostty-vt` without `sudo` into `~/.local` instead:
```bash
zig build -Demit-lib-vt -Doptimize=ReleaseFast --prefix "$HOME/.local"
```
Then pass `-DCMAKE_PREFIX_PATH="$HOME/.local"` when configuring `ghostty-union` below (and ensure `~/.local/lib` is in your runtime linker path or `CMAKE_INSTALL_RPATH`).

### WSL2 / WSLg Notes

`ghostty-union` runs inside **WSL2 with no Linux desktop environment** as long as **WSLg** is enabled (default on Windows 11 and modern Windows 10 WSL2):

- **WSL2 Debian/Ubuntu install order (for humans or coding agents):**
  1. Install the `apt` prerequisites above — specifically including **`libsensors-dev`** (required by `KSysGuard::SystemStats` at link time even though WSL2 lacks hardware temperature sensors) along with `curl` and `xz-utils`.
  2. Build and install `libghostty-vt` using the **official Zig tarball** matching `ghostty`'s `build.zig.zon` (`minimum_zig_version`, currently **`0.17.1`**) via `https://ziglang.org/download/${ZIG_VERSION}/zig-$(uname -m)-linux-${ZIG_VERSION}.tar.xz` — **do not** run `apt install zig` (installs `0.14.x`, which fails).
  3. Return to the `ghostty-union` checkout and run **Configure, Build, and Test** followed by **Install Binary & Desktop Entry** below.
- **No `zsh`, Ghostty, KDE Plasma, or `ksystemstats` required:** It launches `$SHELL` (falling back to `/bin/bash` → `/bin/zsh` → `/bin/sh` if `$SHELL` is unset) and falls back to `TERM=xterm-256color` when Ghostty terminfo/shell-integration is absent.
- **Telemetry bar in WSL2:** Without a running `ksystemstats` daemon on session D-Bus, CPU/RAM/GPU/NET throughput cards show `--` while the terminal, clock, and IP readouts work normally. Hardware temperature sensors and `/proc/driver/nvidia/gpus` are typically unavailable inside the WSL2 VM.
- **Qt backend toggle under WSLg:** If Wayland window decorations or HiDPI scaling look off under WSLg, launch with `QT_QPA_PLATFORM=xcb ghostty-union` (or `QT_QPA_PLATFORM=wayland`).
- **Opening links in Windows:** Install `wslu` so `xdg-open` delegates `Ctrl+Click` URLs to your Windows browser.

### Configure, Build, and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The test suite runs headlessly using `QT_QPA_PLATFORM=offscreen` and covers key encoding, live/mocked telemetry cards, multi-GPU hotplug, split trees and drag-and-drop rearrangement, tab modals, links, `OSC 7` PWD tracking, Ghostty surface parity, and leaked TUI mode sanitization.

### Install Binary & Desktop Entry

User-local install (`~/.local/bin/ghostty-union`, `~/.local/share/applications/ghostty-union.desktop` with resolved absolute `Exec=`/`TryExec=`, and `hicolor` fallback icon):
```bash
cmake --install build --prefix ~/.local
kbuildsycoca6 --noincremental 2>/dev/null || true
```

Or install system-wide via CMake:
```bash
sudo cmake --install build --prefix /usr/local
```

---

## CLI Usage

```bash
ghostty-union                         # Launch and restore saved session layout (or open in current dir)
ghostty-union --no-restore            # Launch a fresh window without restoring saved session layout
ghostty-union -d /path/to/dir         # Launch fresh in a specific working directory
ghostty-union --working-directory ~   # Long flag equivalent
ghostty-union -t solar-forge          # Override terminal theme for this window (solar-forge | ollie)
ghostty-union --theme ollie           # Long flag equivalent
ghostty-union --help                  # Show CLI options
```

---

## Repository Layout

```text
.
├── CMakeLists.txt           # CMake build (C++20, Qt6 6.8+, KSysGuard, libghostty-vt 1.0+)
├── ghostty-union.desktop.in # FreeDesktop launcher template (configured with absolute Exec= path)
├── assets/
│   └── ghostty-union.svg    # Fallback hicolor scalable vector application icon
├── src/
│   ├── main.cpp             # Application entry point & CLI parser
│   ├── MainWindow.{h,cpp}   # Window shell, tabs, shortcuts, Solar Forge modals
│   ├── StatsBarWidget.{h,cpp} # 74px live telemetry cockpit bar (ksystemstats + NET/VPN/clock)
│   ├── TerminalTabPane.{h,cpp} # Dynamic split tree, drag-and-drop swap/dock/detach, zoom
│   ├── TerminalWidget.{h,cpp}  # libghostty-vt surface, PTY, renderer, OSC 133;D sanitization
│   ├── TerminalLink.{h,cpp} # OSC 8 & regex URL/file:line:col resolver and opener
│   ├── KeyMap.{h,cpp}       # Qt QKeyEvent -> GhosttyKeyEvent mapping
│   └── Theme.h              # Solar Forge chrome & terminal palettes (Solar Forge, Ollie)
└── tests/
    └── smoke_test.cpp       # QtTest suite (offscreen QPA)
```
