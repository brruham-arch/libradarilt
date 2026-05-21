// libradarilt - CHud::DrawRadar 3D tilt effect
// Target: libGTASA.so ARM32 Thumb2
// Offsets confirmed via readelf/nm analysis
//
// Hook: CHud::DrawRadar @ 0x00437ABC
// Inject emu_glPushMatrix + emu_glMultMatrixf (tilt) + emu_glPopMatrix
// around the original call to produce a pseudo-3D perspective tilt.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <android/log.h>

#include "mod/amlmod.h"

// ─── Logging ────────────────────────────────────────────────────────────────

#define LOG_TAG   "libradarilt"
#define LOGFILE   "/storage/emulated/0/radarilt_log.txt"

static void _log(const char* msg) {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}

static void _logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _log(buf);
}

// ─── Offsets (dari readelf/nm - sudah verified) ──────────────────────────────
//
// Semua offset RELATIVE to libGTASA.so base.
// Fungsi ARM Thumb2: hook address = base + offset + 1 (Thumb bit)
//
//   CHud::DrawRadar              0x00437ABC  → hook target
//   emu_glPushMatrix             0x001BA74D  → push GL matrix stack
//   emu_glPopMatrix              0x001BA85D  → pop GL matrix stack
//   emu_glMultMatrixf(float*)    0x001BA995  → multiply current matrix
//   emu_glRotate(f,f,f,f,f)     0x001BAB49  → rotate (angle,x,y,z) float ver
//   emu_glTranslate(f,f,f,f)    0x001BAE8D  → translate

#define OFF_DrawRadar    0x00437ABCu
#define OFF_PushMatrix   0x001BA74Du
#define OFF_PopMatrix    0x001BA85Du
#define OFF_MultMatrixf  0x001BA995u
#define OFF_Rotate       0x001BAB49u
#define OFF_Translate    0x001BAE8Du

// ─── Tilt matrix ─────────────────────────────────────────────────────────────
//
// Efek "minimap miring ke depan" = rotasi sumbu X ~25 derajat.
// Pakai OpenGL column-major matrix 4x4.
//
// Rotasi X sebesar angle θ:
//   [ 1    0       0     0 ]
//   [ 0  cos θ  -sin θ   0 ]
//   [ 0  sin θ   cos θ   0 ]
//   [ 0    0       0     1 ]
//
// θ = 25° → cos(25°) ≈ 0.9063, sin(25°) ≈ 0.4226
// Nilai ini bisa diubah via TILT_ANGLE_DEG di bawah.

#define TILT_ANGLE_DEG  25.0f

static float g_tiltMatrix[16];

static void buildTiltMatrix(float deg) {
    float rad  = deg * 3.14159265f / 180.0f;
    float c    = cosf(rad);
    float s    = sinf(rad);

    // Column-major, row index berubah cepat
    g_tiltMatrix[0]  = 1.0f; g_tiltMatrix[4]  = 0.0f; g_tiltMatrix[8]  = 0.0f;  g_tiltMatrix[12] = 0.0f;
    g_tiltMatrix[1]  = 0.0f; g_tiltMatrix[5]  = c;    g_tiltMatrix[9]  = -s;    g_tiltMatrix[13] = 0.0f;
    g_tiltMatrix[2]  = 0.0f; g_tiltMatrix[6]  = s;    g_tiltMatrix[10] = c;     g_tiltMatrix[14] = 0.0f;
    g_tiltMatrix[3]  = 0.0f; g_tiltMatrix[7]  = 0.0f; g_tiltMatrix[11] = 0.0f;  g_tiltMatrix[15] = 1.0f;
}

// ─── Function pointer types ──────────────────────────────────────────────────

typedef void (*tVoid)(void);
typedef void (*tMultMatrixf)(const float*);
typedef void (*tRotatef)(float angle, float x, float y, float z, float unused);
typedef void (*tTranslatef)(float x, float y, float z, float unused);

static tVoid         fn_Push        = nullptr;
static tVoid         fn_Pop         = nullptr;
static tMultMatrixf  fn_MultMatrixf = nullptr;

// Original DrawRadar yang disimpan Dobby
static tVoid orig_DrawRadar = nullptr;

// ─── Hook function ───────────────────────────────────────────────────────────

static void hk_DrawRadar() {
    if (!fn_Push || !fn_Pop || !fn_MultMatrixf || !orig_DrawRadar) {
        // Fallback: panggil original tanpa efek jika pointer belum siap
        if (orig_DrawRadar) orig_DrawRadar();
        return;
    }

    fn_Push();
    fn_MultMatrixf(g_tiltMatrix);
    orig_DrawRadar();
    fn_Pop();
}

// ─── AML MYMOD ───────────────────────────────────────────────────────────────

MYMOD(brruham.libradarilt, libradarilt, 1.0, brruham)

ON_MOD_PRELOAD() {
    // Hapus log lama
    remove(LOGFILE);
    _log("[radarilt] === OnModPreLoad ===");
    _log("[radarilt] v1.0 | CHud::DrawRadar tilt mod | brruham");
}

ON_MOD_LOAD() {
    _log("[radarilt] === OnModLoad mulai ===");

    // ── 1. Ambil base address libGTASA.so ────────────────────────────────────
    void* hGTASA = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!hGTASA) {
        _log("[radarilt] ERROR: dlopen libGTASA.so gagal");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: libGTASA tidak ditemukan");
        return;
    }
    uintptr_t base = (uintptr_t)hGTASA;
    _logf("[radarilt] libGTASA.so base = 0x%08X", (unsigned)base);

    // ── 2. Resolve alamat fungsi GL emu ──────────────────────────────────────
    // Thumb2: semua address + 1 untuk mode indicator,
    // tapi kita simpan sebagai pointer biasa (tanpa +1) untuk call langsung.
    // DobbyHook yang butuh +1, call biasa tidak.

    fn_Push       = (tVoid)       (base + OFF_PushMatrix);
    fn_Pop        = (tVoid)       (base + OFF_PopMatrix);
    fn_MultMatrixf = (tMultMatrixf)(base + OFF_MultMatrixf);

    _logf("[radarilt] emu_glPushMatrix   @ 0x%08X", (unsigned)(base + OFF_PushMatrix));
    _logf("[radarilt] emu_glPopMatrix    @ 0x%08X", (unsigned)(base + OFF_PopMatrix));
    _logf("[radarilt] emu_glMultMatrixf  @ 0x%08X", (unsigned)(base + OFF_MultMatrixf));

    // Validasi pointer tidak null (base 0 = library tidak loaded)
    if (!fn_Push || !fn_Pop || !fn_MultMatrixf) {
        _log("[radarilt] ERROR: satu atau lebih pointer GL null");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: GL pointer null");
        return;
    }
    _log("[radarilt] GL emu pointers OK");

    // ── 3. Build tilt matrix ─────────────────────────────────────────────────
    buildTiltMatrix(TILT_ANGLE_DEG);
    _logf("[radarilt] Tilt matrix built, angle=%.1f deg", TILT_ANGLE_DEG);
    _logf("[radarilt] Matrix[0..3] = %.4f %.4f %.4f %.4f",
          g_tiltMatrix[0], g_tiltMatrix[1], g_tiltMatrix[2], g_tiltMatrix[3]);
    _logf("[radarilt] Matrix[4..7] = %.4f %.4f %.4f %.4f",
          g_tiltMatrix[4], g_tiltMatrix[5], g_tiltMatrix[6], g_tiltMatrix[7]);
    _logf("[radarilt] Matrix[8..11]= %.4f %.4f %.4f %.4f",
          g_tiltMatrix[8], g_tiltMatrix[9], g_tiltMatrix[10], g_tiltMatrix[11]);

    // ── 4. Load Dobby ────────────────────────────────────────────────────────
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) {
        _log("[radarilt] ERROR: dlopen libdobby.so gagal");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: libdobby tidak ditemukan");
        return;
    }
    _log("[radarilt] libdobby.so loaded OK");

    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) {
        _log("[radarilt] ERROR: DobbyHook symbol tidak ditemukan");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: DobbyHook sym null");
        return;
    }
    _log("[radarilt] DobbyHook symbol OK");

    // ── 5. Hook CHud::DrawRadar ──────────────────────────────────────────────
    // Thumb2 hook: address + 1 (Thumb bit)
    void* drawRadarAddr = (void*)(base + OFF_DrawRadar + 1);
    _logf("[radarilt] CHud::DrawRadar target = 0x%08X (offset=0x%08X + base + 1)",
          (unsigned)(uintptr_t)drawRadarAddr, OFF_DrawRadar);

    int hookRet = dobbyHook(
        drawRadarAddr,
        (void*)hk_DrawRadar,
        (void**)&orig_DrawRadar
    );

    if (hookRet != 0) {
        _logf("[radarilt] ERROR: DobbyHook gagal, ret=%d", hookRet);
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: Hook gagal (ret=%d)", hookRet);
        return;
    }

    _logf("[radarilt] Hook sukses! orig_DrawRadar = 0x%08X",
          (unsigned)(uintptr_t)orig_DrawRadar);

    // ── 6. Done ──────────────────────────────────────────────────────────────
    _log("[radarilt] === OnModLoad SELESAI ===");
    if (aml) aml->ShowToast(false, "[RadarTilt] Aktif - tilt %.0f deg", TILT_ANGLE_DEG);
}
