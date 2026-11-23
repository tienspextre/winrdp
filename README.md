<div align="center">

<img src="data/io.winrdp.Client.svg" width="84" height="84" alt="WinRDP logo" />

<h1>WinRDP</h1>

<p>
  <strong>Native Windows <code>mstsc.exe</code>-class Remote Desktop (RDP) client for Ubuntu &amp; Linux</strong><br />
  GTK4 + Libadwaita + OpenGL 3.3 &bull; Bundled FreeRDP 3.33.1 with FFmpeg H.264/AVC444 &amp; Intel VA-API
</p>

<p>
  <a href="https://github.com/tienspextre/winrdp/releases">
    <img src="https://img.shields.io/github/v/release/tienspextre/winrdp?style=flat-square&color=1c71d8" alt="Latest Release" />
  </a>
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square" alt="License Apache-2.0" />
  </a>
  <img src="https://img.shields.io/badge/Ubuntu-20.04%20--%2026.10-E95420?style=flat-square&logo=ubuntu&logoColor=white" alt="Ubuntu 20.04 to 26.10" />
  <img src="https://img.shields.io/badge/Display-Wayland%20%26%20X11-2ec27e?style=flat-square" alt="Wayland and X11" />
  <img src="https://img.shields.io/badge/FreeRDP-3.33.1%20%2B%20H.264%20VA--API-613583?style=flat-square" alt="FreeRDP 3.33.1 + H.264 VA-API" />
</p>

<p>
  <strong>English</strong> &nbsp;|&nbsp; <a href="README.vi.md">Tiếng Việt</a>
</p>

</div>

---

WinRDP is built to eliminate the most common pain points of Linux RDP clients — fullscreen black flicker, stuck Vietnamese IME input, lost keyboard grabs, uncentered windowed title bars, and broken multi-monitor sessions — delivering the responsiveness and polish of Windows `mstsc.exe` natively on **Wayland** and **X11**.

## Table of Contents

1. [Features](#1-features)
2. [Compatibility & Quick Installation](#2-compatibility--quick-installation)
3. [Connection Manager Guide](#3-connection-manager-guide)
4. [Profile Settings Reference (5 Tabs)](#4-profile-settings-reference-5-tabs)
5. [In-Session Guide](#5-in-session-guide)
6. [Keyboard Shortcuts](#6-keyboard-shortcuts)
7. [Command-Line Interface (CLI)](#7-command-line-interface-cli)
8. [Configuration Storage & Troubleshooting](#8-configuration-storage--troubleshooting)
9. [Building from Source & Packaging](#9-building-from-source--packaging)
10. [License](#10-license)

---

## 1. Features

| Category | Capabilities |
| --- | --- |
| **Graphics & Rendering** | Single-window zero-flicker `GtkOverlay` + OpenGL 3.3 dirty-rect (`glTexSubImage2D`), crisp 1:1 `GL_NEAREST` vs Smart Sizing `GL_LINEAR`, bundled FreeRDP `3.33.1` with FFmpeg `H.264/AVC444`, `AVC420`, `RemoteFX` & automatic Intel VA-API (`LIBVA_DRIVER_NAME=iHD`), Dynamic Resolution (`MS-RDPEDISP`). |
| **`mstsc.exe` UX (Wayland & X11)** | Floating top Connection Bar in fullscreen with independent **Pin** & **Hide (Hotkey-Only)** controls and live codec/RTT/FPS telemetry; windowed mode switches seamlessly to a native `AdwHeaderBar` auto-centered in usable workarea below the Ubuntu top bar. |
| **True Multi-Monitor** | Native per-monitor fullscreen (`gtk_window_fullscreen_on_monitor`), cross-display app dragging (including mixed DPI/offsets), live in-session toggle (`Ctrl+Alt+M`), single unified Ubuntu Dock icon, and synchronized raise/minimize/restore across monitors. |
| **Keyboard Grab & Vietnamese IME** | Full system shortcut capture (`Alt+Tab`, `Win`, `PrintScreen`, `Ctrl+Shift+Esc`), single tap-release `Right Ctrl` grab toggle (`Right Ctrl+C/V` stays in Windows), and raw scancode forwarding bypassing `GtkIMContext` so **UniKey/EVKey** inside Windows never conflicts with Ubuntu `ibus`/`fcitx5`. |
| **Credentials & Profiles** | Quick Connect (`user@host:port`, `rdp://`, IPv6), Library filters (**All**, **Favorites**, **Recent**, **Groups**) & instant search, UTF-16LE/UTF-8 `.rdp` Import/Export, smart `DOMAIN\user` / `.\localuser` / UPN normalization + NetBIOS/IP NTLM without Kerberos, **Remember Last Username**, GNOME Keyring (`libsecret`), and Active Sessions switcher. |
| **Device & Network Redirection** | Remote audio playback & microphone redirection, bidirectional text & image clipboard (+ FUSE3 file copy/paste on native `.deb`), Shared Folders (`\\tsclient`), local printers, Auto-Reconnect (1–50 attempts), Network Auto-Detect, and RD Gateway. |
| **Universal Packaging** | Single `.flatpak` bundle running uniformly across Ubuntu 20.04–26.10 & other distros, plus lightweight native `.deb` packages for Ubuntu 24.04 LTS and Ubuntu 26.04 LTS / 26.10. |

---

## 2. Compatibility & Quick Installation

- **Display Servers:** Supports both **Wayland** and **X11** (`x86_64` / `amd64`).
- **Hardware Acceleration (Intel GPU):** Automatically sets `LIBVA_DRIVER_NAME=iHD` when Intel graphics + Intel VA-API drivers (`intel-media-va-driver` on host or `org.freedesktop.Platform.VAAPI.Intel` in Flatpak) are present.
- **Credential Storage:** Uses `gnome-keyring` via `libsecret` (`org.freedesktop.secrets`, pre-installed on Ubuntu Desktop).

### Package Comparison

| Package | Supported OS | Bundled Runtime / Libs | Clipboard File Copy/Paste | Shared Folders (`\\tsclient`) |
| --- | --- | --- | --- | --- |
| **Universal Flatpak** *(Recommended)* | Ubuntu **20.04 – 26.10** & any Flatpak Linux | Self-contained GNOME 50 + FreeRDP 3.33.1 | Text & Images *(sandboxed)* | Yes (`--filesystem=home`) |
| **Native `.deb` (`ubuntu26.04`)** | Ubuntu **26.04 LTS** & **26.10** | Uses system GTK4/FFmpeg + bundled FreeRDP 3.33.1 | Yes (**FUSE3** + Text/Images) | Yes |
| **Native `.deb` (`ubuntu24.04`)** | Ubuntu **24.04 LTS** | Uses system GTK4/FFmpeg + bundled FreeRDP 3.33.1 | Yes (**FUSE3** + Text/Images) | Yes |

### Option 1: Single-File Universal Installer — Flatpak (Recommended)

One `.flatpak` file works across **Ubuntu 20.04, 22.04 LTS, 24.04 LTS, 26.04 LTS, 26.10** and other Linux distributions without system library mismatches:

```bash
# Install Flatpak if not already installed
sudo apt update && sudo apt install -y flatpak

# Download and install WinRDP (the first install pulls the base GNOME runtime from Flathub)
wget https://github.com/tienspextre/winrdp/releases/download/v1.3.0/winrdp_1.3.0_x86_64.flatpak
flatpak install --user -y ./winrdp_1.3.0_x86_64.flatpak
```

Launch **WinRDP** from the Ubuntu App Grid or terminal:

```bash
flatpak run io.winrdp.Client
```

- **Upgrade:** `flatpak install --user -y --reinstall ./winrdp_<version>_x86_64.flatpak`
- **Uninstall:** `flatpak uninstall --user -y io.winrdp.Client`

### Option 2: Native `.deb` Packages (Ubuntu 24.04 LTS / 26.04 LTS / 26.10)

Download the `.deb` matching your Ubuntu release from **[GitHub Releases](https://github.com/tienspextre/winrdp/releases)**:

- **Ubuntu 26.04 LTS / Ubuntu 26.10:**

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.3.0/winrdp_1.3.0_ubuntu26.04_amd64.deb
sudo apt update
sudo apt install ./winrdp_1.3.0_ubuntu26.04_amd64.deb
```

- **Ubuntu 24.04 LTS:**

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.3.0/winrdp_1.3.0_ubuntu24.04_amd64.deb
sudo apt update
sudo apt install ./winrdp_1.3.0_ubuntu24.04_amd64.deb
```

- **Upgrade / Uninstall:** `sudo apt install ./winrdp_<version>_ubuntu<distro>_amd64.deb` or `sudo apt remove winrdp`.

### Verify Checksums (Optional, Applies to Both `.flatpak` & `.deb`)

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.3.0/SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt
```

---

## 3. Connection Manager Guide

Open **WinRDP** from the application launcher or via terminal (`winrdp` or `flatpak run io.winrdp.Client`). The Connection Manager consists of four areas:

### Quick Connect Bar
Located in the top header bar:
- Enter a target in the **Quick Connect** field. Supported formats:
  - Hostname / IP: `192.168.1.50`, `win11-dev`
  - With username & custom port: `user@192.168.1.50`, `user@192.168.1.50:3390`
  - Full URI or domain: `rdp://CORP\user@10.0.0.5:3389`, `DOMAIN/user@host`
  - IPv6: `[fe80::1]:3389`
- Toggle the **Multi-Monitor** icon button beside the input to connect across all monitors (**automatically enabled** when 2 or more displays are connected).
- Press `Enter` or click **Connect**.

### Connection Library & Profiles
- **Create (`+`):** Opens the 5-tab profile editor to save reusable connections.
- **Connect:** Click **Connect** or **double-click** any connection card.
- **Card Actions:** Star (`★` Favorite), Edit (pencil icon), Export `.rdp` (save icon), and Delete (trash icon).
- **Left Sidebar (Library):** Filter by **All Connections**, **Favorites**, **Recent**, or custom **Groups**.
- **Search (`🔍`):** Filter instantaneously by display name, hostname/IP, username, or group.

### Windows `.rdp` Import & Export
- **Import .rdp File (folder icon):** Opens standard `.rdp` files saved by Windows `mstsc.exe`, supporting **UTF-16LE** and **UTF-8** encodings as well as `selectedmonitors`, `displayconnectionbar`, and `use multimon`.
- **Export .rdp (card button):** Exports a saved profile to `.rdp` for backup or sharing with Windows users.

### Active Sessions Bar
When at least one RDP session is running, an **Active Sessions** bar appears directly below the Connection Manager header:
- Click a session pill to **raise and focus (or unminimize)** that session window immediately — including all screens of a multi-monitor session.
- Click `×` on a pill to disconnect that session cleanly.

---

## 4. Profile Settings Reference (5 Tabs)

Click **New Connection (`+`)** or **Edit** to configure all five tabs:

### Tab 1: General
- **Display Name / Group (optional) / Favorite:** Card label, sidebar group name, and favorite pin.
- **PC Name / Host / IP & RDP Port:** Target Windows address and port (default `3389`).
- **Username / Domain (optional) / Password:** Windows credentials. You may enter **Username** and **Domain** separately, or type `DOMAIN\user`, `.\localuser`, or a UPN (`user@corp.local`) directly into **Username** — WinRDP normalizes credentials automatically and uses NTLM directly for NetBIOS domains and IP targets even when Ubuntu has no Kerberos `krb5.conf` configured.
- **Remember Last Username:** Saves the last used Username + Domain upon pressing **Connect** (also works when **Save Password** is disabled so you do not have to re-type your login name each time).
- **Save Password in GNOME Keyring:** Encrypts the password via `libsecret`; passwords are never written in plaintext to JSON.

### Tab 2: Display
- **Display Mode:**
  - `Fullscreen (Single Monitor)`: Fullscreen on one Ubuntu monitor.
  - `Windowed`: Resizable window with native Ubuntu title bar.
  - `Multi-Monitor (Fullscreen)`: Span native fullscreen Windows desktop across multiple displays.
- **Resolution Preset & Desktop Width / Height:** Pick `Custom / Native` or presets (`1920 x 1080`, `2560 x 1440`, `3840 x 2160`, `1600 x 900`, `1280 x 720`).
- **Dynamic Resolution (`MS-RDPEDISP`):** Automatically updates the Windows desktop resolution when resizing the window or toggling Fullscreen/Windowed.
- **Smart Sizing:** Smoothly scales the remote desktop to fit the window. Leave **off** for razor-sharp 1:1 pixels (`GL_NEAREST`), or turn **on** when viewing a larger remote screen inside a smaller window.
- **Remote Windows DPI Scaling:** `Auto`, `100%`, `125%`, `150%`, `200%`.
- **Pin Connection Bar & Hide Connection Bar (Hotkey-Only):** Two independent switches controlling fullscreen Connection Bar visibility:

| `Hide Connection Bar` | `Pin Connection Bar` | Fullscreen Behavior |
| --- | --- | --- |
| **Off** | **Off** | **Auto-Hide:** Hidden by default; hover top-center edge to reveal, hides automatically when pointer leaves. |
| **On** | **Off** | **Hotkey-Only:** Top-edge hover disabled; press **`Ctrl+Alt+Home`** to show, auto-hides after use or on `Ctrl+Alt+Home`. |
| **Off** or **On** | **On** | **Always Visible:** Stays pinned on screen. If **Hide** is also On, unpinning immediately hides the bar and returns to `Ctrl+Alt+Home` mode. |

- **Monitors:**
  - In `Multi-Monitor (Fullscreen)`: Enable **Use All Connected Monitors** to span every local display, or disable it to pick specific monitors individually.
  - In `Fullscreen (Single Monitor)`: Choose **Target Fullscreen Monitor** to lock fullscreen onto a specific Ubuntu display.

### Tab 3: Resources
- **Remote Audio Playback:** `Play on this computer (Ubuntu)`, `Play on remote Windows`, or `Do not play`.
- **Redirect Microphone:** Forward Ubuntu microphone input to Windows apps and calls.
- **Clipboard (Text & Images):** Bidirectional text and image copy/paste.
- **Clipboard File Copy/Paste:** Bidirectional file copy/paste via **FUSE3** *(available on native `.deb` / source builds; in the sandboxed Flatpak build, use **Shared Folders (`\\tsclient`)** for file transfers)*.
- **Redirect Local Printers:** Expose local Ubuntu printers to the Windows session.
- **Capture System Shortcuts (`Win`, `Alt+Tab`, `PrtSc`):** `Only in Fullscreen` *(recommended)*, `Always when focused`, or `Never`.
- **Release Keyboard Grab Key:** Single-tap release/re-grab key (`Right Ctrl`, `Right Alt`, or `Scroll Lock`).
- **Shared Folders (`\\tsclient`):** Click **Add Folder** to mount Ubuntu directories inside Windows File Explorer under `\\tsclient`.

### Tab 4: Experience
- **Graphics Codec:** Keep `Auto (AVC444 -> AVC420 -> RemoteFX)` *(recommended)* or pick `H.264 / AVC444 (Crisp text + 60fps)`, `H.264 / AVC420 (Low bandwidth)`, `RemoteFX (Classic GFX)`, or `Standard GDI (Legacy)`.
- **Hardware Video Decode (Intel VA-API):** Offloads H.264/AVC444 decoding to Intel UHD/Iris GPUs.
- **Visual Options:** Font Smoothing (ClearType), Desktop Composition, Show Window Contents While Dragging, Menu & Window Animations, Desktop Background Wallpaper.
- **Auto-Reconnect on Drop & Max Reconnect Attempts:** Automatically reconnects (1–50 attempts) after transient network drops without leaving fullscreen.
- **Network Auto-Detect (Live Latency/RTT):** Measures real-time round-trip latency and connection quality on the Connection Bar.

### Tab 5: Advanced
- **Security Layer:** `Auto Negotiate`, `NLA (CredSSP — Recommended)`, `TLS`, or `Classic RDP`.
- **Ignore Certificate Warnings:** Automatically accept self-signed TLS certificates *(disable on untrusted networks)*.
- **RD Gateway Server:** Optional Gateway hostname, port (default `443`), and option to share or separate Gateway credentials.

---

## 5. In-Session Guide

### Fullscreen & Connection Bar
In fullscreen mode, the Connection Bar sits cleanly inside the single session overlay:
- **Pin (`📌`):** Keep the bar permanently visible or allow auto-hiding.
- **Signal Icon + Codec Badge:** Hover to inspect live resolution, codec (`AVC444 VA`, `AVC420`, `RFX`), RTT latency (`ms`), and actual `fps`.
- **Keyboard Icon:** Toggle system shortcut capture (`Alt+Tab`, `Win`).
- **Lock Icon (`Send Ctrl+Alt+Del`):** Send `Ctrl+Alt+Del` to the Windows lock/login screen.
- **Zoom Icon (`Smart Sizing`):** Toggle 1:1 sharpness vs window fit scaling.
- **Hide (`^`):** Collapse bar (when unpinned, switches/arms Hotkey-Only reveal via `Ctrl+Alt+Home`).
- **Minimize / Multi-Monitor / Restore (`Windowed`) / Close (`×`).**

### Windowed Mode
Press `Ctrl+Alt+Enter` or click Restore on the Connection Bar:
- WinRDP sizes the window cleanly and centers it inside your **usable workarea** — evenly positioned below the Ubuntu top bar on both **X11** and **Wayland** so the native `AdwHeaderBar` never hides behind system panels.
- The header bar displays live session metrics (resolution, codec, FPS) alongside quick toolbar controls (Keyboard Capture, `Ctrl+Alt+Del`, Smart Sizing, Multi-Monitor, Re-enter Fullscreen).

### Multi-Monitor Workflows
- **Automatic Span on Connect:** Connecting with a multi-monitor profile or Quick Connect Multi-Monitor active expands Windows across your selected Ubuntu monitors automatically.
- **Hot Toggle Without Disconnecting:** Press `Ctrl+Alt+M` (or click the Multi-Monitor button) mid-session to switch between 1 monitor and multi-monitor seamlessly.
- **Cross-Screen Window Dragging:** Drag Windows app windows freely across monitors, even with mixed resolutions, DPI scales, or display offsets.
- **Unified Ubuntu Dock & Group Raising:** All screens of a session appear under **one single window group** on the Ubuntu Dock / `Alt+Tab`. Clicking or focusing any WinRDP monitor brings all monitors of that session forward together; minimizing restores and minimizes them in sync.

### Keyboard Grab & Vietnamese IME (`UniKey` / `EVKey`)
- **Full Windows Shortcuts:** When Keyboard Grab is active (default in Fullscreen), `Alt+Tab`, `Win+D`, `Win+R`, `Ctrl+Shift+Esc`, and `PrintScreen` go directly to Windows.
- **Quick Return to Ubuntu:** **Tap and release `Right Ctrl` alone** to release keyboard grab back to Ubuntu without exiting fullscreen; tap `Right Ctrl` again to resume capture. *(Combos such as `Right Ctrl + C` or `Right Ctrl + V` still execute normally in Windows.)*
- **Clean Vietnamese Input:** Set Ubuntu's host input source (`ibus` / `fcitx5`) to English while focused in WinRDP and type with **UniKey** or **EVKey** directly inside Windows. Because WinRDP dispatches raw scancodes without `GtkIMContext`, characters are never duplicated or dropped.

---

## 6. Keyboard Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl + Alt + Home` *(or `Pause`)* | Show / hide the fullscreen Connection Bar (also reveals in Hotkey-Only mode) |
| `Ctrl + Alt + Enter` | Toggle between **Fullscreen** and **Windowed** mode |
| `Ctrl + Alt + M` | Toggle **Multi-Monitor** mode live inside an active session |
| `Ctrl + Alt + G` | Toggle system key capture (`Win`, `Alt+Tab`, `Ctrl+Shift+Esc`) |
| `Right Ctrl` *(single tap-release)* | Release / re-capture keyboard focus between Ubuntu and Windows *(configurable in Resources)* |

---

## 7. Command-Line Interface (CLI)

Use either **`winrdp`** (native `.deb` / source install) or **`flatpak run io.winrdp.Client`** (Flatpak):

```text
winrdp [options] [file.rdp | rdp://... | user@host[:port]]
```

| Flag | Description |
| --- | --- |
| `-v`, `--version` | Print WinRDP version, bundled FreeRDP version, and H.264/HWAccel status |
| `--list-monitors` | List monitor indices (`[0]`, `[1]`...), connector names, resolutions, coordinates, and DPI scale |
| `-m`, `--multimon` | Force Multi-Monitor Fullscreen mode |
| `--monitors <0,1,...>` | Select specific monitor indices (auto-enables Multi-Monitor when 2 or more are given) |
| `-f`, `--fullscreen` | Force Single-Monitor Fullscreen mode |
| `-w`, `--windowed` | Force Windowed mode with native header bar |
| `--no-bar` | Hide Connection Bar in fullscreen (`Ctrl+Alt+Home` reveals) |
| `--pinned-bar` | Pin Connection Bar always visible in fullscreen |
| `--smoke-test` | Run self-test verifying OpenGL 3.3, dual-canvas multi-monitor, HeaderBar, identity & VA-API |

**Examples:**

```bash
# Native install
winrdp                                  # Launch Connection Manager
winrdp workstation.rdp                  # Connect directly from a .rdp file
winrdp rdp://user@192.168.1.50          # Quick URI connect
winrdp --multimon user@10.0.0.5         # Span across all monitors
winrdp --monitors 0,1 user@10.0.0.5     # Use monitors 0 and 1 (check indices via --list-monitors)
winrdp --fullscreen --no-bar 10.0.0.5   # Fullscreen with Hotkey-Only bar
winrdp --windowed admin@192.168.1.20:3390

# Universal Flatpak install
flatpak run io.winrdp.Client --list-monitors
flatpak run io.winrdp.Client --multimon user@10.0.0.5
flatpak run io.winrdp.Client --smoke-test
```

---

## 8. Configuration Storage & Troubleshooting

### Configuration & Secret Locations

| Item | Native (`.deb` / Source) | Universal Flatpak (`io.winrdp.Client`) |
| --- | --- | --- |
| **Connection Profiles (`profiles.json`)** | `~/.config/winrdp/profiles.json` | `~/.var/app/io.winrdp.Client/config/winrdp/profiles.json` |
| **Saved Passwords** | Encrypted in **GNOME Keyring** (`libsecret`) | Shared host **GNOME Keyring** (`org.freedesktop.secrets`) |

### Common Troubleshooting

1. **Text looks slightly soft or blurry:**
   - Check the **Smart Sizing** button (magnifying glass icon) and turn it **off** for pixel-exact 1:1 rendering (`GL_NEAREST`).
   - Under **Experience**, prefer `Auto` or `H.264 / AVC444 (Crisp text + 60fps)`.
   - Keep **Dynamic Resolution (`MS-RDPEDISP`)** enabled so Windows adopts your exact native display resolution.
2. **Multi-Monitor does not span all displays:**
   - Run `winrdp --list-monitors` (or `flatpak run io.winrdp.Client --list-monitors`) to inspect detected displays and indices.
   - Ensure the remote host runs **Windows Pro / Enterprise / Server** (Windows Home editions do not include an RDP server, and legacy Windows 7/8 lack full `MS-RDPEDISP` multi-monitor support).
3. **Check hardware VA-API & OpenGL 3.3:**
   - Run `winrdp --smoke-test` (or `flatpak run io.winrdp.Client --smoke-test`) and verify `OpenGL initialized: YES`, `H.264/FFmpeg GFX support: YES`, and `LIBVA_DRIVER_NAME: iHD` on Intel GPUs. On native Ubuntu installs, ensure `intel-media-va-driver` (or `intel-media-va-driver-non-free`) is installed.
4. **Domain login (`DOMAIN\user`) fails or uses local credentials:**
   - Leave **Domain** empty for local accounts, or enter `DOMAIN` in **Domain** (or `DOMAIN\user` in **Username**) for AD/NetBIOS domains. Enable **Remember Last Username** to persist the normalized login name without saving your password.
5. **Can't see the window title bar or Ubuntu hotkeys stop responding:**
   - Press `Ctrl+Alt+Enter` to toggle between Fullscreen and Windowed mode.
   - Tap and release `Right Ctrl` once if Keyboard Grab is capturing system keys into Windows.
6. **File copy/paste in Flatpak:**
   - Flatpak isolates the sandbox from host FUSE mounts (`WITH_FUSE=OFF`), so clipboard handles text & images while file transfers use **Shared Folders (`\\tsclient`)** in Tab 3 (**Resources**). If you rely heavily on direct Explorer clipboard file copy/paste on Ubuntu 24.04–26.10, use the native `.deb` package.

---

## 9. Building from Source & Packaging

**Stack:** C++20 &bull; GTK4 &bull; Libadwaita &bull; OpenGL 3.3 (`libepoxy`) &bull; FreeRDP `3.33.1` &bull; `libsecret` &bull; `cJSON` &bull; CMake + Ninja

```bash
./scripts/install-deps.sh                     # Install build dependencies on Ubuntu (run once)
./scripts/build-freerdp3.sh                   # Build bundled FreeRDP 3.33.1 (H.264 + VA-API + Pulse + FUSE3)
./scripts/build-app.sh                        # Compile WinRDP and run all CTest unit tests
./scripts/install-desktop.sh                  # Install local ~/.local/bin binary & desktop integration
./scripts/build-flatpak.sh                    # Package single universal .flatpak bundle (all Ubuntu versions)
./scripts/build-deb.sh                        # Package native .deb for current Ubuntu release
./scripts/build-deb-docker.sh --ubuntu 26.04  # Build & verify native .deb for Ubuntu 26.04 & 26.10 via Docker
```

---

## 10. License

WinRDP is licensed under the **[Apache License 2.0](LICENSE)**.
Bundled third-party components include **FreeRDP 3.33.1** (Apache-2.0).
