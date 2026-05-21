# libradarilt

AML mod untuk SA-MP Mobile — minimap pseudo-3D tilt effect.

## Cara kerja

Hook `CHud::DrawRadar` (offset `0x437ABC` di `libGTASA.so`), inject:
1. `emu_glPushMatrix` — simpan matrix state
2. `emu_glMultMatrixf(tiltMatrix)` — terapkan rotasi X ~25°
3. panggil original `DrawRadar`
4. `emu_glPopMatrix` — restore matrix state

## Ganti sudut tilt

Edit `TILT_ANGLE_DEG` di `mod/main.cpp`:
```cpp
#define TILT_ANGLE_DEG  25.0f   // ubah sesuai selera (10-45 wajar)
```

## Log

`/storage/emulated/0/radarilt_log.txt`

## Build

Push ke GitHub → Actions otomatis build → download artifact `libradarilt-arm32`.

## Install

Taruh `libradarilt.so` di:
```
/storage/emulated/0/Android/data/com.sampmobilerp.game/mods/
```
