// libradarilt v1.2 - diagnostic + brute force approach
// Jika TransformRadarPointToScreenSpace tidak efektif,
// kita hook DrawRadarMap + DrawBlips langsung dan modif posisi gambar.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <android/log.h>

#include "mod/amlmod.h"

#define LOG_TAG  "libradarilt"
#define LOGFILE  "/storage/emulated/0/radarilt_log.txt"

static void _log(const char* msg) {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}
static void _logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    _log(buf);
}

// ─── Offsets ─────────────────────────────────────────────────────────────────
// Semua confirmed dari readelf/nm sesi analisis

#define OFF_TransformRPtoSS   0x0043F5DCu  // void(CVector2D&, const CVector2D&)
#define OFF_DrawRadarMap      0x0043E5A0u  // void()
#define OFF_DrawBlips         0x0043E99Cu  // void(float)
#define OFF_DrawRadarMask     0x00444150u  // void()
#define OFF_DrawRadarSection  0x004437B0u  // void(int,int,int)
#define OFF_m_radarRect       0x00994DB0u  // CRect {x1,y1,x2,y2}
#define OFF_vec2DRadarOrigin  0x00994DA4u  // CVector2D

struct CVector2D { float x, y; };
struct CRect     { float x1, y1, x2, y2; };

// ─── Global state ─────────────────────────────────────────────────────────────

static uintptr_t g_base      = 0;
static uint32_t  g_callCount = 0;   // counter: berapa kali hook dipanggil
static bool      g_logged    = false;

// ─── Tilt params ─────────────────────────────────────────────────────────────
// Pendekatan: modifikasi m_radarRect sebelum DrawRadarMap dipanggil,
// kembalikan setelah selesai.
// Ini memaksa game menggambar radar di area yang sudah di-shear.
//
// Alternatif: patch vec2DRadarOrigin untuk geser origin.
//
// TILT_Y_SHRINK: kompres tinggi radar dari atas (0.0-1.0)
//   0.7 = atas radar hanya 70% dari posisi normal → terlihat miring

#define TILT_Y_SHRINK  0.65f   // semakin kecil = semakin miring

// ─── Hook: TransformRadarPointToScreenSpace ───────────────────────────────────
// Diagnostic: hitung berapa kali dipanggil, log sample pertama

typedef void (*tTransform)(CVector2D& out, const CVector2D& in);
static tTransform orig_Transform = nullptr;

static void hk_Transform(CVector2D& out, const CVector2D& in) {
    orig_Transform(out, in);

    g_callCount++;

    // Log 3 sampel pertama untuk verifikasi
    if (g_callCount <= 3) {
        _logf("[radarilt] Transform called #%u: in=(%.3f,%.3f) out=(%.3f,%.3f)",
              g_callCount, in.x, in.y, out.x, out.y);
    }

    // Terapkan tilt: kompres Y dari atas
    // Baca radar rect untuk dapat center
    if (!g_base) return;
    float* r = (float*)(g_base + OFF_m_radarRect);
    float y1 = r[1], y2 = r[3];
    if (y2 <= y1) return;

    float radarH  = y2 - y1;
    float centerY = (y1 + y2) * 0.5f;

    // t = posisi vertikal dalam radar (0=atas, 1=bawah)
    float t = (out.y - y1) / radarH;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Scale: atas dikompres, bawah normal
    float scale = TILT_Y_SHRINK + (1.0f - TILT_Y_SHRINK) * t;
    float dy    = out.y - centerY;
    out.y       = centerY + dy * scale;
}

// ─── Hook: DrawRadarMap (diagnostic) ─────────────────────────────────────────

typedef void (*tVoid)(void);
static tVoid orig_DrawRadarMap = nullptr;

static void hk_DrawRadarMap() {
    if (!g_logged && g_base) {
        float* r = (float*)(g_base + OFF_m_radarRect);
        _logf("[radarilt] DrawRadarMap called! radarRect=(%.1f,%.1f,%.1f,%.1f)",
              r[0], r[1], r[2], r[3]);
        CVector2D* origin = (CVector2D*)(g_base + OFF_vec2DRadarOrigin);
        _logf("[radarilt] vec2DRadarOrigin=(%.3f,%.3f)", origin->x, origin->y);
        g_logged = true;
    }
    orig_DrawRadarMap();
}

// ─── AML ─────────────────────────────────────────────────────────────────────

MYMOD(brruham.libradarilt, libradarilt, 1.2, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === OnModPreLoad v1.2 ===");
}

ON_MOD_LOAD() {
    _log("[radarilt] === OnModLoad mulai ===");

    // Base address
    uintptr_t base = 0;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps) { _log("[radarilt] ERROR: /proc/self/maps"); return; }
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libGTASA.so") && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoul(line, nullptr, 16);
                _logf("[radarilt] base = 0x%08X", (unsigned)base);
                break;
            }
        }
        fclose(maps);
    }
    if (!base) { _log("[radarilt] ERROR: base not found"); return; }
    g_base = base;

    // Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { _log("[radarilt] ERROR: libdobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { _log("[radarilt] ERROR: DobbyHook sym"); return; }
    _log("[radarilt] Dobby OK");

    // Hook 1: TransformRadarPointToScreenSpace
    {
        void* t = (void*)(base + OFF_TransformRPtoSS + 1);
        int r = dobbyHook(t, (void*)hk_Transform, (void**)&orig_Transform);
        _logf("[radarilt] Hook Transform: ret=%d orig=0x%08X",
              r, (unsigned)(uintptr_t)orig_Transform);
    }

    // Hook 2: DrawRadarMap (diagnostic saja)
    {
        void* t = (void*)(base + OFF_DrawRadarMap + 1);
        int r = dobbyHook(t, (void*)hk_DrawRadarMap, (void**)&orig_DrawRadarMap);
        _logf("[radarilt] Hook DrawRadarMap: ret=%d orig=0x%08X",
              r, (unsigned)(uintptr_t)orig_DrawRadarMap);
    }

    _log("[radarilt] === OnModLoad SELESAI ===");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.2 Aktif");
}
