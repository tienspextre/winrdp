# WinRDP — Hướng dẫn sử dụng & Trải nghiệm RDP chuẩn Windows `mstsc.exe` trên Ubuntu

Ứng dụng Remote Desktop (RDP) native cho Ubuntu (GTK4 + Libadwaita + OpenGL 3.3), xây dựng trực tiếp trên lõi **FreeRDP 3.33.1** với **FFmpeg H.264/AVC444 + Intel VA-API**.

WinRDP được thiết kế để khắc phục các điểm khó chịu thường gặp của RDP client trên Linux (chớp màn hình khi fullscreen, kẹt bộ gõ tiếng Việt, rớt grab phím hoặc vỡ đa màn hình), mang lại trải nghiệm mượt mà tương tự `mstsc.exe` trên Windows và Thincast.

## Mục lục

1. [Điểm nổi bật](#điểm-nổi-bật)
2. [Yêu cầu hệ thống & Cài đặt nhanh](#yêu-cầu-hệ-thống--cài-đặt-nhanh)
3. [Hướng dẫn sử dụng giao diện Connection Manager](#hướng-dẫn-sử-dụng-giao-diện-connection-manager)
4. [Giải thích chi tiết 5 tab cấu hình Profile](#giải-thích-chi-tiết-5-tab-cấu-hình-profile)
5. [Hướng dẫn thao tác trong phiên Remote Desktop](#hướng-dẫn-thao-tác-trong-phiên-remote-desktop)
6. [Phím tắt mặc định](#phím-tắt-mặc-định)
7. [Sử dụng qua dòng lệnh CLI](#sử-dụng-qua-dòng-lệnh-cli)
8. [Vị trí lưu dữ liệu & Xử lý sự cố thường gặp](#vị-trí-lưu-dữ-liệu--xử-lý-sự-cố-thường-gặp)
9. [Build từ mã nguồn & Đóng gói .deb](#build-từ-mã-nguồn--đóng-gói-deb)

---

## Điểm nổi bật

- **Không nháy màn hình (Zero-Flicker Single-Window OpenGL 3.3):**
  - Toàn bộ khung hình RDP + thanh Connection Bar + thông báo kết nối lại nằm trong **1 `GtkOverlay` duy nhất** — không mở cửa sổ popup X11 phụ khi fullscreen nên GNOME Mutter không bị chớp đen.
  - Upload GPU theo từng vùng thay đổi (*dirty-rect* `glTexSubImage2D`), tự dùng `GL_NEAREST` khi chạy đúng tỷ lệ 1:1 (chữ ClearType sắc nét) và `GL_LINEAR` khi bật Smart Sizing.
- **Connection Bar chuẩn `mstsc.exe` + Header Bar ở chế độ cửa sổ:**
  - Fullscreen dùng thanh **Connection Bar** nổi ở cạnh trên với 2 công tắc độc lập (**Pin/Ghim** và **Hide/Hotkey-Only** via `Ctrl+Alt+Home`); khi thoát sang chế độ cửa sổ tự động chuyển sang thanh tiêu đề gốc đặt gọn trong vùng làm việc (*workarea*, không bị lẹm lên thanh panel trên cùng).
- **Bắt phím Windows & Gõ tiếng Việt chuẩn:**
  - Bắt trọn `Alt+Tab`, phím `Win`, `PrintScreen`, `Ctrl+Shift+Esc` khi đang focus vào Windows.
  - Không gắn `GtkIMContext` lên vùng hiển thị RDP → gõ UniKey/EVKey trong Windows bình thường mà không bị `ibus`/`fcitx5` của Ubuntu nuốt phím.
- **Đa màn hình thực sự (X11 & Wayland):**
  - Tự tạo cửa sổ fullscreen gốc trên từng màn hình (`gtk_window_fullscreen_on_monitor`), hỗ trợ kéo cửa sổ ứng dụng Windows xuyên suốt qua lại giữa các màn, bật/tắt nhanh trong phiên (`Ctrl+Alt+M`), gộp chung 1 cửa sổ trên Ubuntu Dock và tự nâng đồng bộ toàn bộ màn hình khi quay lại phiên remote.

---

## Yêu cầu hệ thống & Cài đặt nhanh

- **Hệ điều hành:** **Ubuntu 24.04 LTS**, **Ubuntu 26.04 LTS** và **Ubuntu 26.10** (`amd64`) — hỗ trợ cả phiên **Wayland** và **X11**.
- **Tăng tốc phần cứng (tùy chọn cho GPU Intel):** `intel-media-va-driver-non-free` hoặc `intel-media-va-driver` (tự nhận biến `LIBVA_DRIVER_NAME=iHD` khi phát hiện GPU Intel).
- **Lưu mật khẩu:** `gnome-keyring` (cài sẵn trên Ubuntu Desktop).

### Cài đặt từ gói `.deb` phát hành sẵn

Tải đúng gói `.deb` cho phiên bản Ubuntu của bạn tại trang **[GitHub Releases](https://github.com/tienspextre/winrdp/releases)** (tất cả gói đều đóng gói sẵn **FreeRDP 3.33.1** hỗ trợ **H.264/AVC444 + VA-API** riêng trong `/usr/lib/winrdp/lib/`, không xung đột với `libfreerdp` hệ thống):

- **Ubuntu 26.04 LTS / Ubuntu 26.10 (mới nhất):**

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.2.0/winrdp_1.2.0_ubuntu26.04_amd64.deb
sudo apt update
sudo apt install ./winrdp_1.2.0_ubuntu26.04_amd64.deb
```

- **Ubuntu 24.04 LTS:**

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.2.0/winrdp_1.2.0_ubuntu24.04_amd64.deb
sudo apt update
sudo apt install ./winrdp_1.2.0_ubuntu24.04_amd64.deb
```

Kiểm tra toàn vẹn SHA-256 (tùy chọn):

```bash
wget https://github.com/tienspextre/winrdp/releases/download/v1.2.0/SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt
```

- **Nâng cấp phiên bản mới:** tải file `.deb` mới hơn và chạy lại `sudo apt install ./winrdp_<phiên_bản>_ubuntu<bản>_amd64.deb` — toàn bộ profile và mật khẩu đã lưu được giữ nguyên.
- **Gỡ cài đặt:** `sudo apt remove winrdp`.

---

## Hướng dẫn sử dụng giao diện Connection Manager

Sau khi mở **WinRDP** từ menu ứng dụng Ubuntu hoặc chạy lệnh `winrdp`, màn hình quản lý kết nối gồm 4 khu vực chính:

### 1. Kết nối nhanh (Quick Connect)
Ở thanh trên cùng của cửa sổ quản lý:
- Nhập địa chỉ máy Windows vào ô **Quick Connect**, hỗ trợ các định dạng:
  - IP / Tên máy: `192.168.1.50`, `win11-dev`
  - Kèm tài khoản & cổng: `user@192.168.1.50`, `user@192.168.1.50:3390`
  - Kèm domain & URI: `rdp://CORP\user@10.0.0.5:3389` hoặc `DOMAIN/user@host`
  - IPv6: `[fe80::1]:3389`
- Bấm nút biểu tượng **Multi-Monitor** bên cạnh ô nhập để bật/tắt kết nối toàn bộ màn hình (mặc định **tự bật sẵn** nếu máy Ubuntu đang cắm từ 2 màn hình trở lên).
- Nhấn `Enter` hoặc bấm **Connect**.

### 2. Quản lý & Sắp xếp Profile
- **Tạo mới (`+`):** mở hộp thoại cấu hình 5 tab và lưu lại để dùng lâu dài.
- **Kết nối:** bấm nút **Connect** hoặc **double-click** vào thẻ kết nối.
- **Sửa / Yêu thích / Xóa:** trên mỗi thẻ có nút sao yêu thích (`★`), nút sửa (biểu tượng bút), nút xuất `.rdp` và nút xóa (biểu tượng thùng rác).
- **Cột trái (Library):** lọc theo **All Connections**, **Favorites**, **Recent**, hoặc từng **Group** bạn đặt tên trong profile.
- **Tìm kiếm (`🔍`):** lọc nhanh theo tên profile, hostname/IP, username hoặc group.

### 3. Nhập & Xuất file `.rdp` Windows
- **Import .rdp File (biểu tượng mở thư mục):** đọc trực tiếp file `.rdp` xuất từ `mstsc.exe` trên Windows (hỗ trợ cả mã hóa **UTF-16LE**, **UTF-8**, và tham số `selectedmonitors`, `displayconnectionbar`, `use multimon`).
- **Export .rdp (biểu tượng lưu trên từng thẻ):** xuất profile ra file `.rdp` để mở trên máy Windows hoặc sao lưu.

### 4. Thanh Active Sessions (Chuyển nhanh về phiên đang chạy)
Khi có ít nhất một phiên RDP đang kết nối, thanh **Active Sessions** xuất hiện ngay dưới thanh tiêu đề của Connection Manager:
- Bấm vào tên phiên để **đưa thẳng cửa sổ remote lên trước / khôi phục nếu đang thu nhỏ** (kể cả khi dùng nhiều màn hình).
- Bấm nút `×` cạnh tên phiên để ngắt kết nối nhanh.

---

## Giải thích chi tiết 5 tab cấu hình Profile

Khi bấm **New Connection (`+`)** hoặc **Edit Connection**, hộp thoại có 5 tab:

### 1. Tab **General**
- **Display Name / Group / Favorite:** tên hiển thị, tên nhóm gom ở sidebar trái và ghim vào mục yêu thích.
- **PC Name / Host / IP & RDP Port:** địa chỉ máy đích và cổng (mặc định `3389`).
- **Username / Domain / Password:** thông tin đăng nhập Windows. Hỗ trợ cả nhập tách riêng **Username** + **Domain** hoặc gõ trực tiếp dạng `DOMAIN\user` / `.\localuser` / UPN `user@domain.vn` ở ô Username (tự chuẩn hóa sạch trước khi xác thực và dùng NTLM trực tiếp cho Domain NetBIOS / IP khi máy Ubuntu chưa cấu hình Kerberos).
- **Remember Last Username:** tự lưu lại Username + Domain vừa dùng gần nhất khi bấm **Connect** (hỗ trợ cả khi **tắt** lưu mật khẩu để lần sau không phải gõ lại tên đăng nhập).
- **Save Password in GNOME Keyring:** lưu mật khẩu mã hóa qua `libsecret` (không bao giờ lưu chữ thô trong file JSON).

### 2. Tab **Display**
- **Display Mode:**
  - `Fullscreen (Single Monitor)`: toàn màn hình trên 1 màn hình Ubuntu.
  - `Windowed`: chạy trong cửa sổ thông thường có thanh tiêu đề để kéo/resize.
  - `Multi-Monitor (Fullscreen)`: mở rộng Windows lên nhiều màn hình Ubuntu.
- **Resolution Preset & Desktop Width/Height:** chọn độ phân giải khởi tạo (có sẵn preset `1920x1080`, `2560x1440`, `3840x2160`, `1600x900`, `1280x720` hoặc nhập tay).
- **Dynamic Resolution (`MS-RDPEDISP`):** tự động đổi độ phân giải desktop Windows cho vừa khít khi bạn kéo cỡ cửa sổ hoặc chuyển Fullscreen/Windowed.
- **Smart Sizing:** co giãn hình ảnh Windows để vừa khung cửa sổ (khuyên **tắt** khi muốn chữ 1:1 sắc nét nhất, hoặc bật khi xem màn hình remote độ phân giải lớn trong cửa sổ nhỏ).
- **Remote Windows DPI Scaling:** `Auto`, `100%`, `125%`, `150%`, `200%`.
- **Pin Connection Bar & Hide Connection Bar (Hotkey-Only):** hai công tắc độc lập điều khiển thanh Connection Bar khi fullscreen:
  - `Hide = Tắt, Pin = Tắt` (**Auto-hide**): mặc định ẩn, rê chuột vào mép trên giữa màn hình sẽ trượt xuống và tự ẩn khi rời chuột.
  - `Hide = Bật, Pin = Tắt` (**Hotkey-Only**): tắt kích hoạt bằng chuột ở mép trên; **chỉ dùng `Ctrl+Alt+Home`** để hiện thanh (sau khi hiện sẽ tự ẩn lại nếu không tương tác / rời chuột hoặc bấm `Ctrl+Alt+Home` lần nữa).
  - `Pin = Bật` *(bất kể `Hide` bật hay tắt)*: thanh Connection Bar **luôn hiển thị**. Nếu bật cả `Hide` và `Pin`, thanh vẫn luôn hiện cho tới khi bạn bỏ ghim (`Unpin`), lúc đó sẽ ẩn ngay và quay về chế độ `Ctrl+Alt+Home`.
- **Monitors:**
  - Khi chọn `Multi-Monitor`: giữ bật **Use All Connected Monitors** để dùng tất cả màn hình, hoặc tắt đi để tích chọn riêng từng màn hình cụ thể.
  - Khi chọn `Fullscreen (Single Monitor)`: mục **Target Fullscreen Monitor** cho phép cố định màn hình Ubuntu nào sẽ hiển thị phiên remote.

### 3. Tab **Resources**
- **Remote Audio Playback:** phát âm thanh Windows ra loa Ubuntu (`Play on this computer`), để trên máy từ xa, hoặc tắt tiếng.
- **Redirect Microphone:** chuyển micro của Ubuntu vào Windows để họp/gọi.
- **Clipboard (Text & Images) & Clipboard File Copy/Paste:** copy/paste văn bản, hình ảnh và file 2 chiều (chép file thông qua FUSE3).
- **Redirect Local Printers:** chia sẻ máy in cục bộ vào phiên Windows.
- **Capture System Shortcuts:** chọn lúc nào phím hệ thống (`Win`, `Alt+Tab`, `PrintScreen`, `Ctrl+Shift+Esc`) được gửi thẳng vào Windows: `Only in Fullscreen` (khuyên dùng), `Always when focused` (kể cả lúc windowed), hoặc `Never`.
- **Release Keyboard Grab Key:** chọn phím bấm-nhả đơn để thả/bắt lại bàn phím (`Right Ctrl`, `Right Alt`, hoặc `Scroll Lock`).
- **Shared Folders (`\\tsclient`):** bấm **Add Folder** để chia sẻ thư mục Ubuntu vào Windows; bên trong Windows mở File Explorer và gõ `\\tsclient` để truy cập.

### 4. Tab **Experience**
- **Graphics Codec:** khuyên giữ `Auto (AVC444 -> AVC420 -> RemoteFX)` hoặc chọn `H.264 / AVC444` (chữ sắc nét + 60fps). Nếu mạng rất chậm có thể chọn `AVC420` hoặc `RemoteFX`.
- **Hardware Video Decode (Intel VA-API):** giảm tải CPU bằng GPU Intel UHD/Iris.
- **Tùy chọn hiển thị Windows:** ClearType Font Smoothing, Desktop Composition, Show Window Contents While Dragging, Menu & Window Animations, Desktop Background Wallpaper.
- **Auto-Reconnect on Drop & Max Reconnect Attempts:** tự động kết nối lại tối đa 1–50 lần khi rớt mạng tạm thời mà không làm thoát fullscreen.
- **Network Auto-Detect:** đo độ trễ (RTT ms) và chất lượng đường truyền hiển thị trên Connection Bar.

### 5. Tab **Advanced**
- **Security Layer:** `Auto Negotiate`, `NLA (CredSSP — Recommended)`, `TLS`, hoặc `Classic RDP`.
- **Ignore Certificate Warnings:** bỏ qua hộp thoại xác nhận chứng chỉ TLS tự ký (nên tắt nếu kết nối máy công cộng không tin cậy).
- **RD Gateway Server:** cấu hình địa chỉ Gateway, cổng (mặc định `443`), và tùy chọn dùng chung hoặc tách riêng tài khoản Gateway.

---

## Hướng dẫn thao tác trong phiên Remote Desktop

### 1. Chế độ Fullscreen & Thanh Connection Bar
Khi vào Fullscreen, hành vi thanh **Connection Bar** phụ thuộc 2 nút **Pin (Ghim)** và **Hide (Mũi tên lên)**:
- **Auto-hide (`Pin` tắt, `Hide` tắt):** rê chuột lên sát **mép trên giữa màn hình** để trượt thanh xuống; rời chuột thanh tự ẩn.
- **Hotkey-Only Hide (`Pin` tắt, `Hide` bật):** mặc định ẩn và không hiện khi rê chuột; nhấn `Ctrl+Alt+Home` để mở/đóng nhanh (và tự ẩn lại sau khi dùng xong).
- **Ghim (`Pin` bật, dù `Hide` bật hay tắt):** thanh **luôn hiển thị**. Nếu vừa bật `Hide` vừa bật `Pin`, sau khi bỏ ghim thanh sẽ tự ẩn ngay về chế độ `Ctrl+Alt+Home`.
- **Biểu tượng sóng + huy hiệu Codec:** rê chuột để xem chi tiết độ phân giải, codec (`AVC444 VA`, `AVC420`, `RFX`), độ trễ RTT (ms) và FPS thực tế.
- **Bàn phím (Capture):** bật/tắt bắt phím hệ thống (`Alt+Tab`, `Win`).
- **Ổ khóa (`Send Ctrl+Alt+Del`):** gửi tổ hợp `Ctrl+Alt+Del` tới màn hình đăng nhập/khóa Windows.
- **Kính lúp (`Smart Sizing`):** chuyển nhanh giữa tỷ lệ nét 1:1 và co giãn vừa khung hình.
- **Thu nhỏ / Đa màn hình / Thoát Fullscreen / Đóng (`X`).**

### 2. Chế độ Windowed (Cửa sổ có thanh tiêu đề)
Nhấn `Ctrl+Alt+Enter` hoặc nút Restore trên Connection Bar để thoát sang chế độ cửa sổ:
- Cửa sổ tự thu kích thước gọn và căn **chính giữa màn hình** trong vùng làm việc usable (*workarea*, cách đều dưới thanh panel trên cùng của Ubuntu — kể cả trên **X11 đơn/đa màn hình** lẫn **Wayland**) để phần thanh tiêu đề gốc (`AdwHeaderBar`) luôn thoáng và kéo di chuyển dễ dàng.
- Phía trên thanh tiêu đề hiển thị độ phân giải/codec/FPS hiện tại cùng bộ nút nhanh (Capture bàn phím, `Ctrl+Alt+Del`, Smart Sizing, Multi-Monitor, vào lại Fullscreen).

### 3. Sử dụng Nhiều màn hình (Multi-Monitor)
- **Tự bật khi kết nối:** nếu profile chọn `Multi-Monitor` hoặc Quick Connect đang bật nút Multi-Monitor, WinRDP tự mở rộng Windows ra tất cả màn hình đã chọn ngay khi kết nối xong.
- **Bật/tắt nóng ngay trong phiên:** nhấn `Ctrl+Alt+M` hoặc bấm nút Multi-Monitor để chuyển qua lại giữa 1 màn hình và nhiều màn hình mà không phải ngắt kết nối.
- **Kéo thả ứng dụng Windows giữa các màn:** nắm thanh tiêu đề bất kỳ app Windows nào và kéo thẳng sang màn hình kia (hỗ trợ cả khi 2 màn khác độ phân giải, khác tỷ lệ DPI hoặc đặt lệch trái/phải).
- **Chuyển đổi giữa Ubuntu và Windows nhiều màn hình:**
  - Trên **Ubuntu Dock / Alt+Tab**, cả phiên đa màn hình được gom gọn thành **1 cửa sổ phiên remote** (không tách lẻ 2–3 cửa sổ).
  - Khi bạn chuyển sang dùng một ứng dụng Ubuntu ở màn 1 rồi muốn quay lại Windows, chỉ cần **bấm chuột hoặc Alt+Tab vào bất kỳ màn hình nào** của WinRDP — toàn bộ các màn hình còn lại của phiên remote sẽ tự động nổi lên lại cùng lúc.
  - Thu nhỏ (`Super+H` hoặc nút Minimize) và khôi phục cũng đồng bộ trên tất cả các màn hình.

### 4. Bắt phím Windows & Gõ tiếng Việt chuẩn
- Khi **Keyboard Grab** đang bật (mặc định tự bật ở Fullscreen): mọi tổ hợp như `Alt+Tab`, `Win+D`, `Win+R`, `Ctrl+Shift+Esc`, `PrintScreen` đều đi thẳng vào Windows.
- Nếu muốn bấm phím `Alt+Tab` hay `Super` ra ngoài Ubuntu mà không cần thoát Fullscreen: **bấm và thả ngay phím `Right Ctrl`** (không giữ cùng phím khác). Bấm `Right Ctrl` một lần nữa để bắt phím trở lại vào Windows. *(Khi gõ `Right Ctrl + C`, `Right Ctrl + V`..., phím Ctrl phải vẫn gửi vào Windows bình thường.)*
- **Gõ tiếng Việt:** tắt hoặc chuyển bộ gõ Ubuntu (`ibus` / `fcitx5`) về tiếng Anh khi focus cửa sổ WinRDP và dùng UniKey/EVKey trực tiếp bên trong Windows — WinRDP truyền phím thô không qua `GtkIMContext` nên không bị nhân đôi ký tự hay kẹt dấu.

---

## Phím tắt mặc định

| Phím tắt | Chức năng |
| --- | --- |
| `Ctrl + Alt + Home` *(hoặc `Pause`)* | Ẩn / hiện nhanh thanh Connection Bar (hoạt động cả khi đang ẩn hoàn toàn) |
| `Ctrl + Alt + Enter` | Chuyển đổi nhanh giữa **Fullscreen** và **Windowed** |
| `Ctrl + Alt + M` | Bật / tắt chế độ **Nhiều màn hình (Multi-Monitor)** ngay trong phiên |
| `Ctrl + Alt + G` | Bật / tắt bắt phím hệ thống (`Win`, `Alt+Tab`, `Ctrl+Shift+Esc`) |
| `Right Ctrl` *(bấm nhả đơn)* | Nhả / bắt lại bàn phím giữa Ubuntu và Windows (đổi phím được trong tab Resources) |

---

## Sử dụng qua dòng lệnh CLI

Cú pháp chung: `winrdp [tùy_chọn] [file.rdp | rdp://... | user@host[:port]]`

| Cờ dòng lệnh | Mô tả |
| --- | --- |
| `-v`, `--version` | In phiên bản WinRDP, FreeRDP và trạng thái hỗ trợ H.264/VA-API |
| `--list-monitors` | Liệt kê chỉ số (`[0]`, `[1]`...), tên cổng kết nối, độ phân giải, tọa độ và tỷ lệ DPI từng màn hình Ubuntu |
| `-m`, `--multimon` | Bắt buộc mở phiên ở chế độ nhiều màn hình (Multi-Monitor Fullscreen) |
| `--monitors <0,1,...>` | Chỉ chọn các chỉ số màn hình cụ thể (tự bật Multi-Monitor nếu chọn ≥ 2 màn hình) |
| `-f`, `--fullscreen` | Bắt buộc mở toàn màn hình trên 1 màn hình |
| `-w`, `--windowed` | Bắt buộc mở ở chế độ cửa sổ có thanh tiêu đề |
| `--no-bar` | Ẩn hoàn toàn Connection Bar khi fullscreen (`Ctrl+Alt+Home` để mở lại) |
| `--pinned-bar` | Ghim thanh Connection Bar luôn hiển thị khi fullscreen |
| `--smoke-test` | Chạy bài kiểm tra tự động OpenGL 3.3, dual-canvas đa màn hình, HeaderBar, identity và VA-API |

**Ví dụ:**

```bash
winrdp                              # Mở giao diện Connection Manager
winrdp server.rdp                   # Kết nối trực tiếp bằng file .rdp
winrdp rdp://user@192.168.1.50      # Kết nối nhanh qua URI RDP
winrdp --multimon user@10.0.0.5     # Kết nối tràn toàn bộ màn hình Ubuntu
winrdp --monitors 0,1 user@10.0.0.5 # Chỉ dùng màn hình 0 và 1 (xem chỉ số bằng --list-monitors)
winrdp --fullscreen --no-bar 10.0.0.5
winrdp --windowed admin@192.168.1.20:3390
```

---

## Vị trí lưu dữ liệu & Xử lý sự cố thường gặp

### Vị trí lưu cấu hình
- **Danh sách kết nối:** `~/.config/winrdp/profiles.json`
- **Mật khẩu:** lưu mã hóa trong **GNOME Keyring** (`libsecret`), không ghi vào file JSON.

### Xử lý sự cố nhanh

1. **Chữ trên Windows hơi mờ hoặc bị răng cưa nhẹ:**
   - Kiểm tra xem nút **Smart Sizing** (biểu tượng kính lúp) có đang bật không: hãy **tắt Smart Sizing** để hiển thị 1:1 theo từng pixel (`GL_NEAREST`).
   - Trong tab **Experience**, ưu tiên codec `Auto` hoặc `H.264 / AVC444 (Crisp text + 60fps)`.
   - Kiểm tra **Dynamic Resolution** đang bật để Windows tự khớp đúng độ phân giải thật của màn hình Ubuntu.
2. **Đa màn hình không tự nhận đủ màn hình mong muốn:**
   - Chạy `winrdp --list-monitors` trên terminal để kiểm tra Ubuntu có đang nhận đủ các màn hình vật lý và tọa độ không.
   - Đảm bảo phiên Windows phía server là Windows Pro / Enterprise / Server (các bản Home hoặc Windows 7/8 không hỗ trợ đầy đủ đa màn hình `MS-RDPEDISP` hoặc không cho phép RDP server).
3. **Kiểm tra tăng tốc phần cứng VA-API & OpenGL:**
   - Chạy `winrdp --smoke-test` để xác nhận `OpenGL initialized: YES`, `H.264/FFmpeg GFX support: YES` và `LIBVA_DRIVER_NAME: iHD` (trên GPU Intel). Nếu thiếu driver VA-API Intel, cài thêm `sudo apt install intel-media-va-driver-non-free` hoặc `intel-media-va-driver`.
4. **Không thấy thanh tiêu đề khi đang chạy hoặc không gõ phím trên Ubuntu được:**
   - Nhấn `Ctrl+Alt+Enter` để chuyển giữa Fullscreen và Windowed.
   - Bấm thả phím `Right Ctrl` nếu đang bị bắt phím vào Windows.

---

## Build từ mã nguồn & Đóng gói `.deb`

```bash
./scripts/install-deps.sh                     # Cài đặt thư viện build & phụ thuộc (chạy 1 lần)
./scripts/build-freerdp3.sh                   # Build FreeRDP 3.33.1 (H.264 + VA-API + Pulse + FUSE3)
./scripts/build-app.sh                        # Biên dịch WinRDP & chạy toàn bộ CTest unit tests
./scripts/install-desktop.sh                  # Tích hợp nhanh vào ~/.local/bin & menu ứng dụng Ubuntu
./scripts/build-deb.sh                        # Đóng gói .deb cho Ubuntu đang chạy (dist/winrdp_<ver>_ubuntu<XX.XX>_amd64.deb)
./scripts/build-deb-docker.sh --ubuntu 26.04  # Build & kiểm tra cài đặt sạch .deb cho Ubuntu 26.04 LTS & 26.10 qua Docker
```
