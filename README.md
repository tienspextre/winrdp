# WinRDP — Trải nghiệm RDP chuẩn Windows `mstsc.exe` & Thincast trên Ubuntu

Ứng dụng Remote Desktop (RDP) native cho Ubuntu (GTK4 + Libadwaita + OpenGL 3.3), xây dựng trực tiếp trên lõi **FreeRDP 3.33.1** (cùng nền tảng với Thincast Remote Desktop Client) với **FFmpeg + H.264/AVC444 + Intel VA-API**.

## Điểm nổi bật (Khắc phục triệt để nhược điểm client Linux truyền thống)

- **Không nháy màn hình (Zero-Flicker Single-Window OpenGL):**
  - Toàn bộ khung hình RDP + thanh Connection Bar + thông báo kết nối lại nằm trong **1 cửa sổ `GtkOverlay` duy nhất** — không tạo cửa sổ popup X11 phụ khi fullscreen nên GNOME Mutter không bao giờ chớp đen.
  - Upload OpenGL 3.3 theo vùng thay đổi (*dirty-rect* `glTexSubImage2D`) đồng bộ VSync theo màn hình, tự chọn `GL_NEAREST` khi chạy 1:1 (chữ ClearType sắc nét) và `GL_LINEAR` khi Smart Sizing.
- **Connection Bar y hệt Windows `mstsc.exe`:**
  - 3 chế độ: **Tự ẩn (Auto-hide)** trượt mượt khi đưa chuột vào mép trên giữa, **Ghim (Pinned)** luôn hiện, hoặc **Ẩn hoàn toàn (`displayconnectionbar:i:0`)** để bấm tab trình duyệt trên Windows không bao giờ bị vướng.
  - Hiển thị tín hiệu mạng, RTT (ms), FPS, codec thực tế (`AVC444 VA`, `AVC420`, `RFX`), nút gửi `Ctrl+Alt+Del`, bật/tắt bắt phím, thu nhỏ, chuyển Fullscreen/Windowed, ngắt kết nối.
- **Bắt phím Windows & Gõ tiếng Việt chuẩn:**
  - Chụp toàn bộ `Alt+Tab`, phím `Win`, `PrintScreen`, `Ctrl+Shift+Esc` gửi thẳng vào Windows khi focus.
  - Không gắn `GtkIMContext` lên vùng RDP → gõ UniKey/EVKey trong Windows hoàn toàn mượt mà, không bị `fcitx5`/`ibus` của Ubuntu can thiệp.
- **Đầy đủ tính năng:**
  - Hardware H.264/AVC444 qua Intel `iHD` VA-API, Dynamic Resolution (`MS-RDPEDISP` debounce 250ms), Con trỏ phần cứng RGBA, Clipboard 2 chiều (chữ, HTML, ảnh, file qua FUSE3), Âm thanh/Mic, Chia sẻ thư mục (`\\tsclient`), **Đa màn hình thực sự (`UseMultimon` + `gtk_window_fullscreen_on_monitor` trên cả X11 & Wayland)** (chọn toàn bộ hoặc từng màn hình cụ thể, bật/tắt live bằng `Ctrl+Alt+M`), Tự kết nối lại không làm văng fullscreen, Lưu mật khẩu an toàn bằng GNOME Keyring (`libsecret`), Hỗ trợ đọc/xuất file `.rdp` (kể cả UTF-16LE và `selectedmonitors` từ Windows).

## Cài đặt nhanh trên Ubuntu 24.04 (`amd64`)

Tải file `.deb` mới nhất tại trang **[GitHub Releases](https://github.com/tienspextre/winrdp/releases)** (đã đóng gói sẵn **FreeRDP 3.33.1** bật **H.264/AVC444 + VA-API** trong `/usr/lib/winrdp/lib/`, không xung đột với thư viện hệ thống) rồi cài bằng `apt`:

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.1.1/winrdp_1.1.1_amd64.deb
sudo apt update
sudo apt install ./winrdp_1.1.1_amd64.deb
```

## Phím tắt mặc định trong phiên RDP

| Phím tắt | Chức năng |
| --- | --- |
| `Ctrl + Alt + Home` hoặc `Ctrl + Alt + Pause` | Ẩn / hiện nhanh thanh Connection Bar (kể cả ở chế độ ẩn hoàn toàn) |
| `Ctrl + Alt + M` | Bật / tắt chế độ **Nhiều màn hình (Multi-Monitor)** ngay trong phiên |
| `Right Ctrl` (bấm nhả đơn) | Thả / bắt lại bàn phím giữa Ubuntu và Windows |
| `Ctrl + Alt + Enter` | Chuyển nhanh Fullscreen / Windowed |

## Build từ mã nguồn & Đóng gói `.deb`

```bash
./scripts/install-deps.sh      # Chỉ cần chạy 1 lần
./scripts/build-freerdp3.sh    # Build FreeRDP 3.33.1 (H.264 + VA-API + Pulse + FUSE3)
./scripts/build-app.sh         # Biên dịch WinRDP & chạy CTest unit tests
./scripts/install-desktop.sh   # Cài đặt nhanh vào ~/.local/bin & menu ứng dụng Ubuntu
./scripts/build-deb.sh         # Đóng gói file .deb vào dist/winrdp_<ver>_amd64.deb
```

## Sử dụng qua dòng lệnh

```bash
winrdp                              # Mở Connection Manager
winrdp server.rdp                   # Kết nối ngay bằng file .rdp
winrdp rdp://user@192.168.1.50      # Kết nối nhanh
winrdp --multimon user@10.0.0.5     # Kết nối spanning nhiều màn hình
winrdp --monitors 0,1 user@10.0.0.5 # Chỉ dùng màn hình 0 và 1
winrdp --list-monitors              # Liệt kê chỉ số màn hình & độ phân giải hiện tại
winrdp --fullscreen --no-bar 10.0.0.5
winrdp --smoke-test                 # Kiểm tra OpenGL 3.3, multi-canvas, FreeRDP 3.33.1 và UI
```
