# Soul Knight — Linux handheld port (NextOS so-loader)

Runs the Android version of **Soul Knight 4.3.8** (`com.ChillyRoom.DungeonShooter`,
Unity 2020.3.26f1, IL2CPP) natively on AArch64 Linux handhelds (muOS, ArkOS,
dArkOS, ROCKNIX, Knulli...) through PortMaster. The loader `soulknight-nextos`
loads the game's own `libmain.so`, `libunity.so` and `libil2cpp.so` and emulates
the Android/JNI/UnityPlayer side; video goes through EGL/GLES2, audio through SDL2.

No game files are included. You must supply the APK yourself.

## Requirements

- An **AArch64 (64-bit ARM)** handheld with PortMaster installed.
- About **1.5 GB of free space** on the card holding `ports/` (the first launch
  extracts ~450 MB of assets and unpacks the AssetBundles to ~800 MB).
- At least ~1.5 GB RAM + swap. On low-RAM devices (e.g. RK3326 with ~1 GB) the
  port adds zram or a swapfile automatically for the session.
- **Soul Knight 4.3.8, 64-bit (arm64-v8a)** — see below.

## Which game file to use

Use exactly this file:

```
com.ChillyRoom.DungeonShooter_4.3.8.apkm
```

This is the APKMirror bundle of version **4.3.8** that contains the
**arm64-v8a** split (base APK + `split_config.arm64_v8a.apk` + asset splits).

Do **not** use:

- **Version 5.x or newer** (including 8.x). These embed Tencent ACE anti-cheat
  (`libanort.so`, `libanogs.so`, `libu8t.so`) and will not run with this port.
- **32-bit only packages** such as `soul-knight-v4.3.8.apk` that contain only
  `armeabi-v7a`. The port needs the `arm64-v8a` libraries.

A `.xapk` or a single APK also works as long as it is version 4.3.8 and
includes `lib/arm64-v8a/`.

## Installation

### Option 1 — PortMaster autoinstall (recommended)

1. Copy `soulknight.zip` to PortMaster's autoinstall folder, e.g. on muOS:
   `/mnt/mmc/MUOS/PortMaster/autoinstall/`
   (use `/mnt/sdcard/...` if your ports live on the second card).
2. Open PortMaster once; it installs the port automatically.
3. Copy `com.ChillyRoom.DungeonShooter_4.3.8.apkm` into
   `ports/soulknight/gamedata/`
   (on muOS usually `/mnt/mmc/ports/soulknight/gamedata/`).

### Option 2 — Manual copy

Copy the port files into your CFW's ports folder (`ROMS/Ports/`,
`roms/ports/`, `/mnt/mmc/ports/`... depending on the CFW):

```
ports/
├── Soul Knight.sh
└── soulknight/
    ├── gamedata/
    │   └── com.ChillyRoom.DungeonShooter_4.3.8.apkm   <- your game file
    ├── nxextract/
    ├── home/
    ├── soulknight-nextos
    ├── port-env.sh
    └── ...
```

Then refresh the game list in your frontend and launch **Soul Knight** from Ports.

## First launch

The first launch takes a few minutes and shows an extraction screen:

1. NXExtract reads the APKM (base + arm64 split + asset splits) and extracts
   `lib/` (the three native libraries) and all of `assets/`.
2. `merge-splits.py` joins the `sharedassetsN.assets.splitM` pieces.
3. `unpack-bundles.py` stores the LZMA AssetBundles uncompressed, so the game
   does not have to inflate them in RAM on every load.
4. On low-RAM devices a swap image may be created once (~1 min).

When it finishes, the game starts. After a successful install you can delete
the `.apkm` from `gamedata/` to free space.

## Controls

The gamepad is exposed to Unity as an Xbox controller, which Soul Knight
supports natively. The on-screen touch buttons are hidden by the bundled
`home/setting.data`.

| Input          | Action                                  |
|----------------|-----------------------------------------|
| Gamepad        | Native Soul Knight controller layout    |
| Right analog   | Moves a pointer on touch-only screens   |
| R1 / R3        | Tap at the pointer                      |
| L2 + R2        | Swap D-pad and Left joystick (toggled)  |
| Select + Start | Quit back to the frontend               |

## Saves

Saves are stored in `ports/soulknight/home/` (`game.data`, `item_data.data`,
`setting.data`, ...). Back up this folder to keep your progress. Older
versions of the port wrote saves to `/`; they are moved into `home/`
automatically on launch.

## Troubleshooting

Logs are written to the port folder:

- `soulknight/log.txt` — current run (see the `RUNTIME DIAGNOSIS` line near the end)
- `soulknight/log.prev.txt` — previous run
- `soulknight/nxextract.log` — first-launch extraction

Common problems:

- **"game data installation did not complete"** — the file in `gamedata/` is
  missing, is not 4.3.8, or has no `arm64-v8a` libraries. Check `nxextract.log`.
- **Not enough space** — free ~1.5 GB on the ports card and launch again.
- **Exit status 137 / game closes while loading the first dungeon** — the
  kernel ran out of memory. Check the `[sk-mem]` lines in `log.txt` to see
  whether swap could be enabled on your CFW.
- **Game freezes** — a watchdog ends a frame stuck for 120 s and returns to the
  menu; the stack trace is written to `log.txt`.

When reporting a bug, include `log.txt` and the name of your device and CFW.

## Known limitations

- Only `libmain`, `libunity` and `libil2cpp` are loaded. Other Android plugins
  (AVPro Video, WebRTC, Audio360, Google Play Games) are not available, so
  videos, online multiplayer and voice chat do not work. Play offline.
- No Google Play Games, ads or in-app purchases.
- The loader was written for Unity 2022.3; this game uses 2020.3.26f1. A shim
  (`libsk_jni_alias.so`, source in `src/sk_jni_alias.c`) bridges the
  differences, but some issues may remain.

## License

The loader is GPL-3.0 and includes Apache-2.0 code; NXExtract is MIT. See
`soulknight/LICENSE`, `soulknight/NOTICE.md` and `soulknight/licenses/`.
Soul Knight and all of its game data are property of ChillyRoom. This is an
independent interoperability project, not affiliated with or endorsed by
ChillyRoom or Unity Technologies.
