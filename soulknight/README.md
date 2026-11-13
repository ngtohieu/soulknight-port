# Soul Knight — port muOS (NextOS so-loader)

Port Android `com.ChillyRoom.DungeonShooter` **4.3.8** (Unity 2020.3.26f1,
IL2CPP, arm64-v8a) cho máy cầm tay Linux AArch64 (muOS, ArkOS, ROCKNIX...).

Loader `soulknight-nextos` là loader Unity/IL2CPP của port **Skateboard Party 3**
trong NextOS Universal Ports (GPL-3.0), giống port Pixel Cup Soccer. Loader nạp
`libmain.so`, `libunity.so`, `libil2cpp.so` gốc của APK, tự giả lập JNI,
bionic và vòng đời UnityPlayer; hình qua EGL/GLES2, tiếng qua SDL2.

## Chỉ dùng bản 4.3.8 (hoặc bản cũ khác không có ACE)

Từ bản 5.x, Soul Knight gắn Tencent ACE (`libanort.so`, `libanogs.so`,
`libu8t.so`) vào `libil2cpp.so`/`libunity.so`; các bản đó không chạy được
với port này. Cần gói có `arm64-v8a`, ví dụ APKMirror
`com.ChillyRoom.DungeonShooter_4.3.8.apkm` (arm64-v8a + arm-v7a). File
`soul-knight-v4.3.8.apk` chỉ có armeabi-v7a (32-bit) cũng **không** dùng được.

## Cài đặt trên muOS

Cách 1 — PortMaster autoinstall:

1. Chép `soulknight.zip` vào `/mnt/mmc/MUOS/PortMaster/autoinstall/`
   (hoặc `/mnt/sdcard/...` nếu dùng thẻ thứ 2).
2. Mở PortMaster một lần để nó tự cài.
3. Chép file `.apkm` vào `ports/soulknight/gamedata/`
   (thường là `/mnt/mmc/ports/soulknight/gamedata/`).

Cách 2 — chép tay:

```
ROMS/Ports/Soul Knight.sh
ROMS/Ports/soulknight/              (cả thư mục)
ROMS/Ports/soulknight/gamedata/com.ChillyRoom.DungeonShooter_4.3.8.apkm
```

## Lần chạy đầu

NXExtract đọc gói APKM (base + split arm64 + split assets), giải nén
`lib/` và toàn bộ `assets/` (~450 MB). Hook `nxextract/merge-splits.py` ghép
các file `sharedassetsN.assets.splitM` thành file liền. Cần khoảng
**1,5 GB trống**. Sau khi cài xong có thể xoá file APKM trong `gamedata/`.

## Nhật ký lỗi

- `soulknight/log.txt` — lần chạy hiện tại (dòng `RUNTIME DIAGNOSIS` ở cuối)
- `soulknight/log.prev.txt` — lần trước
- `soulknight/nxextract.log` — bước giải nén

## Điều khiển

Gamepad được Unity đọc như tay cầm Xbox. Soul Knight có hỗ trợ tay cầm sẵn.
Analog phải điều khiển con trỏ cho các màn hình cảm ứng, R3/R1 = chạm.
- **L2 + R2**: Đổi qua lại (swap) giữa **D-pad** và **cần điều khiển trái (Left Joystick)**. Bấm lại L2 + R2 để quay về bình thường (cài đặt được tự động lưu lại cho các lần chơi sau).
- **Select + Start** để thoát.

## Giới hạn đã biết

- **Chưa được thử trên máy thật.** Loader viết cho Unity 2022.3, game này là
  2020.3.26f1; giao diện JNI của UnityPlayer gần giống nhau nhưng có thể còn
  lệch. Nếu lỗi, gửi `log.txt`.
- Chỉ `libmain`, `libunity`, `libil2cpp` được nạp. Các plugin Android khác
  (AVPro Video, WebRTC, Audio360, Google Play Games) không có — video và
  chơi mạng/voice sẽ không hoạt động; chơi offline.
- Không có Google Play Games, quảng cáo, mua hàng.
