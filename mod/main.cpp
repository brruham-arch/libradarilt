// libradarilt - minimap pseudo-3D tilt
// Target: libGTASA.so ARM32 Thumb2
//
// Strategi: hook TransformRadarPointToScreenSpace
// Fungsi ini konversi koordinat radar (normalized) → screen pixel.
// Kita inject shear transform di sini untuk efek tilt perspektif.
//
// CRadar::TransformRadarPointToScreenSpace @ 0x0043F5DC
// void (CVector2D& out, const CVector2D& in)

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <android/log.h>

#include "mod/amlmod.h"

// ─── Logging ─────────────────────────────────────────────────────────────────

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

// ─── Offset ──────────────────────────────────────────────────────────────────
//
// TransformRadarPointToScreenSpace @ 0x0043F5DC (confirmed dari readelf)
// DrawRadar                        @ 0x00437ABC (untuk referensi)
// m_radarRect                      @ 0x00994DB0 (CRect: x1,y1,x2,y2 float)

#define OFF_TransformRPtoSS  0x0043F5DCu
#define OFF_m_radarRect      0x00994DB0u

// ─── CVector2D struct ─────────────────────────────────────────────────────────

struct CVector2D {
    float x, y;
};

// ─── Tilt parameter ───────────────────────────────────────────────────────────
//
// Efek pseudo-3D: bagian atas minimap "jauh", bagian bawah "dekat".
// Implementasi via perspective-like Y compression + shear:
//
//   t = (in.y + 1.0) / 2.0   (0 = atas, 1 = bawah, normalized dari [-1,1])
//   scale_y = SCALE_TOP + (SCALE_BOT - SCALE_TOP) * t
//   out.y = center_y + (in.y * scale_y * half_h)
//   out.x = center_x + (in.x * half_w)             (X tidak berubah)
//
// TILT_SCALE_TOP: skala Y di bagian atas (lebih kecil = terlihat "jauh")
// TILT_SCALE_BOT: skala Y di bagian bawah (lebih besar = terlihat "dekat")

#define TILT_SCALE_TOP  0.55f   // atas terkompresi (jauh)
#define TILT_SCALE_BOT  1.00f   // bawah normal (dekat)

// ─── State: posisi & ukuran radar di layar ────────────────────────────────────
// Diambil dari m_radarRect saat pertama kali hook dipanggil.

static float g_cx = 0.0f;   // center X radar di screen
static float g_cy = 0.0f;   // center Y radar di screen
static float g_hw = 0.0f;   // half width
static float g_hh = 0.0f;   // half height
static bool  g_rectReady = false;
static uintptr_t g_base = 0;

static void updateRadarRect() {
    if (!g_base) return;
    // m_radarRect = CRect { float x1, y1, x2, y2 }
    float* r = (float*)(g_base + OFF_m_radarRect);
    float x1 = r[0], y1 = r[1], x2 = r[2], y2 = r[3];
    if (x2 <= x1 || y2 <= y1) return;
    g_cx = (x1 + x2) * 0.5f;
    g_cy = (y1 + y2) * 0.5f;
    g_hw = (x2 - x1) * 0.5f;
    g_hh = (y2 - y1) * 0.5f;
    g_rectReady = true;
}

// ─── Hook ─────────────────────────────────────────────────────────────────────

typedef void (*tTransform)(CVector2D& out, const CVector2D& in);
static tTransform orig_Transform = nullptr;

static void hk_Transform(CVector2D& out, const CVector2D& in) {
    // Panggil original dulu → dapat koordinat screen asli
    orig_Transform(out, in);

    if (!g_rectReady) updateRadarRect();
    if (!g_rectReady) return;

    // in.y range: -1 (atas) sampai +1 (bawah) dalam radar space
    // t = 0 di atas, 1 di bawah
    float t = (in.y + 1.0f) * 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float scaleY = TILT_SCALE_TOP + (TILT_SCALE_BOT - TILT_SCALE_TOP) * t;

    // Terapkan scale Y relatif terhadap center radar
    float dy = out.y - g_cy;
    out.y = g_cy + dy * scaleY;
}

// ─── AML setup ───────────────────────────────────────────────────────────────

MYMOD(brruham.libradarilt, libradarilt, 1.1, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === OnModPreLoad ===");
    _log("[radarilt] v1.1 | TransformRadarPointToScreenSpace tilt | brruham");
}

ON_MOD_LOAD() {
    _log("[radarilt] === OnModLoad mulai ===");

    // ── 1. Base address dari /proc/self/maps ──────────────────────────────────
    uintptr_t base = 0;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps) {
            _log("[radarilt] ERROR: gagal buka /proc/self/maps");
            return;
        }
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libGTASA.so") && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoul(line, nullptr, 16);
                _logf("[radarilt] maps hit: %s", line);
                break;
            }
        }
        fclose(maps);
    }
    if (!base) {
        _log("[radarilt] ERROR: libGTASA.so tidak ditemukan di maps");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: base not found");
        return;
    }
    _logf("[radarilt] base = 0x%08X", (unsigned)base);
    g_base = base;

    // ── 2. Log m_radarRect address (baca nanti saat hook dipanggil) ───────────
    _logf("[radarilt] m_radarRect addr = 0x%08X", (unsigned)(base + OFF_m_radarRect));

    // ── 3. Load Dobby ─────────────────────────────────────────────────────────
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) {
        _log("[radarilt] ERROR: libdobby.so tidak ditemukan");
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: libdobby");
        return;
    }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) {
        _log("[radarilt] ERROR: DobbyHook sym null");
        return;
    }
    _log("[radarilt] Dobby OK");

    // ── 4. Hook TransformRadarPointToScreenSpace ──────────────────────────────
    void* target = (void*)(base + OFF_TransformRPtoSS + 1); // +1 Thumb
    _logf("[radarilt] Transform target = 0x%08X", (unsigned)(uintptr_t)target);

    int ret = dobbyHook(target, (void*)hk_Transform, (void**)&orig_Transform);
    if (ret != 0) {
        _logf("[radarilt] ERROR: DobbyHook gagal ret=%d", ret);
        if (aml) aml->ShowToast(true, "[RadarTilt] ERROR: hook gagal");
        return;
    }
    _logf("[radarilt] Hook OK! orig = 0x%08X", (unsigned)(uintptr_t)orig_Transform);

    // ── 5. Done ───────────────────────────────────────────────────────────────
    _logf("[radarilt] Tilt: top=%.2f bot=%.2f", TILT_SCALE_TOP, TILT_SCALE_BOT);
    _log("[radarilt] === OnModLoad SELESAI ===");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.1 Aktif");
}
