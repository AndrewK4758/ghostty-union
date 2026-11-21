# Ghostty Union

A native Qt6 Widgets Linux terminal emulator powered by **`libghostty-vt` (1.0+)**, featuring a **Solar Forge / Ghostty Ollie** cockpit UI, dynamic nested split trees with drag-and-drop rearrangement, and a mounted **74px live system telemetry bar**.

## Features

### Terminal Engine (`libghostty-vt 1.0+`)
- **Full VT & protocol support:** Truecolor (24-bit RGB), Ghostty **Ollie** palette, Kitty Keyboard Protocol & `modifyOtherKeys`, SGR mouse tracking, focus events, bracketed paste, synchronized output holds (mode `2026`), and double/dotted/dashed/curly underlines.
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
- Custom Solar Forge modals for tab renaming and tab close confirmation, plus adaptive tab bar sizing.

---

## Keyboard Shortcuts

### Window & Tabs
| Shortcut | Action |
|---|---|
| `Ctrl+Shift+N` | New window (inherits active working directory) |
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
| `Ctrl+Shift+K` | Soft reset terminal input & display modes (keeps scrollback) |
| `Ctrl+Shift+Up` / `Ctrl+Shift+Down` | Jump to previous / next semantic prompt (`OSC 133`) |
| `Shift+PageUp` / `Shift+PageDown` | Scroll half-page up / down |
| `Ctrl++` / `Ctrl+=` / `Ctrl+Shift++` | Zoom font in |
| `Ctrl+-` | Zoom font out |
| `Ctrl+0` | Reset font size |
| `Ctrl+Backspace` | Erase previous word (`0x17`) |

---

## Platform & Desktop Environment Support

**Short answer:** Yes — `ghostty-union` runs on **any modern Linux distribution** and **any X11 or Wayland desktop environment or window manager** (GNOME, Hyprland, Sway, i3, Cosmic, XFCE, Cinnamon, KDE Plasma, etc.). It is **not** locked to Arch Linux or KDE Plasma.

### What Is Required vs. Optional

| Component | Scope | Notes |
|---|---|---|
| **Linux (POSIX PTY)** | Required | Uses Linux `<pty.h>` (`openpty`, `fork`, `setsid`, `TIOCSCTTY`). macOS and Windows are not currently supported without porting the PTY and telemetry layers. |
| **Qt 6.8+** (`Core`, `Gui`, `Widgets`, `Network`, `DBus`, `Test`) | Required | Pure Qt6 Widgets app — no dependency on KWin, Plasma Shell, `KXmlGui`, or `KParts`. Older LTS distros (e.g., Ubuntu 24.04 with Qt 6.4) need a newer Qt6 toolchain (Ubuntu 25.04+/26.04, Fedora 41+, Debian 13/Trixie+, Arch, NixOS). |
| **`libghostty-vt` >= 1.0** | Required | Packaged directly on Arch (`libghostty-vt`). On other Linux distros, build and install `libghostty-vt` from [`ghostty-org/ghostty`](https://github.com/ghostty-org/ghostty) into `/usr` or `/usr/local`. |
| **`libksysguard`** (`KSysGuard::SystemStats`, `KSysGuard::Formatter`) | Required at build time | Standalone KDE library packaged on most distros (`libksysguard` on Arch, `libksysguard-devel` on Fedora, `libksysguard-dev` on Debian/Ubuntu) without needing a Plasma session. |
| **`ksystemstats` daemon** | Optional at runtime | Provides live CPU, RAM, GPU, and network throughput telemetry over session D-Bus (`org.kde.ksystemstats1`) and can run headless under GNOME/Hyprland/Sway/i3. Without it, the terminal works normally and those sensor cards simply show `--`. |
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
- `libksysguard` (development headers + `KSysGuard` CMake config) and optionally `ksystemstats` at runtime
- `libghostty-vt` >= 1.0 (headers + `libghostty-vt.so.1`; discovered via `ghostty-vt-config.cmake` or `/usr[/local]/{include,lib}`)

#### Arch Linux
```bash
sudo pacman -S --needed cmake base-devel qt6-base libksysguard ksystemstats libghostty-vt
```
*(Optional for shell integration and icons: `ghostty-shell-integration ghostty-terminfo ttf-firacode-nerd`)*

#### Fedora (41+)
```bash
sudo dnf install cmake gcc-c++ qt6-qtbase-devel libksysguard-devel ksystemstats
# Then build/install libghostty-vt >= 1.0 from ghostty-org/ghostty into /usr/local
```

#### Debian (13/Trixie+) / Ubuntu (25.04+)
```bash
sudo apt install cmake g++ qt6-base-dev libksysguard-dev ksystemstats
# Then build/install libghostty-vt >= 1.0 from ghostty-org/ghostty into /usr/local
```

### Configure, Build, and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The test suite runs headlessly using `QT_QPA_PLATFORM=offscreen` and covers key encoding, live/mocked telemetry cards, multi-GPU hotplug, split trees and drag-and-drop rearrangement, tab modals, links, `OSC 7` PWD tracking, Ghostty surface parity, and leaked TUI mode sanitization.

### Install Binary & Desktop Entry

```bash
# User-local install (example)
ln -sf "$(pwd)/build/ghostty-union" ~/.local/bin/ghostty-union
install -Dm644 ghostty-union.desktop ~/.local/share/applications/ghostty-union.desktop
```

Or install system-wide via CMake:
```bash
sudo cmake --install build --prefix /usr/local
```

---

## CLI Usage

```bash
ghostty-union                         # Launch in current working directory
ghostty-union -d /path/to/dir         # Launch in a specific working directory
ghostty-union --working-directory ~   # Long flag equivalent
ghostty-union --help                  # Show CLI options
```

---

## Repository Layout

```text
.
├── CMakeLists.txt           # CMake build (C++20, Qt6 6.8+, KSysGuard, libghostty-vt 1.0+)
├── ghostty-union.desktop    # FreeDesktop application launcher entry
├── src/
│   ├── main.cpp             # Application entry point & CLI parser
│   ├── MainWindow.{h,cpp}   # Window shell, tabs, shortcuts, Solar Forge modals
│   ├── StatsBarWidget.{h,cpp} # 74px live telemetry cockpit bar (ksystemstats + NET/VPN/clock)
│   ├── TerminalTabPane.{h,cpp} # Dynamic split tree, drag-and-drop swap/dock/detach, zoom
│   ├── TerminalWidget.{h,cpp}  # libghostty-vt surface, PTY, renderer, OSC 133;D sanitization
│   ├── TerminalLink.{h,cpp} # OSC 8 & regex URL/file:line:col resolver and opener
│   ├── KeyMap.{h,cpp}       # Qt QKeyEvent -> GhosttyKeyEvent mapping
│   └── Theme.h              # Solar Forge chrome & Ghostty Ollie color palette
└── tests/
    └── smoke_test.cpp       # QtTest suite (offscreen QPA)
```
