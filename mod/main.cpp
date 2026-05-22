// libradarilt v1.6
// CHud::DrawRadar membaca radar rect dari struct[r8] offset 32,36,40,44
// (x1,y1,x2,y2 sebagai float).
// Kita hook DrawRadar, modif y1 sementara untuk efek tilt, restore setelah.
//
// Dari disassembly:
//   r0 = global ptr chain → r0[0x284] = r8 = radar struct
//   r8[32] = x1, r8[36] = y1, r8[40] = x2, r8[44] = y2  (screen coords)
//
// Tilt: naikkan y1 (tepi atas) → atas radar "menyempit" → kesan miring

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <android/log.h>
#include "mod/amlmod.h"

#define LOG_TAG "libradarilt"
#define LOGFILE "/storage/emulated/0/radarilt_log.txt"

static void _log(const char* msg) {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}
static void _logf(const char* fmt, ...) {
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    _log(buf);
}

// ─── Offsets ──────────────────────────────────────────────────────────────────
#define OFF_DrawRadar    0x00437ABCu
// Global pointer chain dari DrawRadar disassembly:
//   ldr r0, [pc+0x4ac] → add r0,pc → ldr r0,[r0] → r0[0x284] = radar_struct
// Kita ambil global pointer ini secara langsung dari binary.
// Dari 437b32: ldr r0,[pc,#0x4ac] → PC=0x437b36, offset=0x4ac → addr=0x437FE2
// Isi 0x437FE2 = 0x00240019 (little-endian) → relative offset
// Terlalu risky parse manual. Lebih aman: baca dari r8 saat hook dipanggil.
// Kita hook DrawRadar, tapi kita TIDAK punya akses r8 langsung di C hook.
//
// Solusi: hook SETELAH prolog, atau simpan pointer saat DrawBlips dipanggil
// (DrawBlips dipanggil di akhir DrawRadar, r8 masih valid di scope DrawRadar).
//
// ALTERNATIF LEBIH SIMPEL:
// Hook DrawRadarMap (dipanggil dari DrawRadar).
// Di dalam DrawRadarMap, baca m_radarRect via pointer chain yang sama.
// Patch y1 sebelum DrawRadarMap, restore sesudah → tile radar ter-tilt.
// Blip ikut ter-tilt karena TransformRPtoSS juga baca rect yang sama.

// Dari disassembly TransformRPtoSS cabang 0x43f618:
//   ldr r2,[pc+0x74] → PC=0x43f61c, +0x74 = 0x43f690
//   isi 0x43f690 = ?? → add r2,pc → ldr r2,[r2] → r2[0x284] = radar_struct
// Sama persis dengan DrawRadar. Ini SATU global pointer.
//
// Kita resolve pointer ini saat runtime via DobbySymbolResolver atau
// baca langsung dari /proc/self/maps + parse offset dari binary.
//
// CARA PALING ROBUST: patch inline di hook DrawRadarMap.
// Kita punya base. Baca 4 byte di 0x43F690 (PC-relative pointer table):
//   val = *(uint32_t*)(base + 0x43F690)  (ini relative offset dari PC 0x43F692)
//   ptr_addr = 0x43F692 + val  (PC at that instruction)
//   global_ptr = *(uint32_t*)(base + ptr_addr)
//   radar_struct = *(uint32_t*)(global_ptr + 0x284)
//   y1 = *(float*)(radar_struct + 36)
//   y2 = *(float*)(radar_struct + 44)

#define OFF_DrawRadarMap  0x0043E5A0u
#define OFF_DrawBlips     0x0043E99Cu

// PC-relative pointer resolve untuk global radar ptr
// Dari 0x43f618: ldr r2,[pc,#0x74] → PC=0x43f61c → target=0x43f690
// 0x43f690 berisi nilai relative ke PC 0x43f692
#define OFF_PTR_TABLE     0x0043F690u  // lokasi 4-byte relative value
#define PC_AT_PTR         0x0043F692u  // PC saat instruksi itu (thumb: addr+4 tapi ldr pc-rel = align4)

// Tilt: berapa persen tinggi radar yang "diambil" dari atas
// 0.3 = tepi atas naik 30% dari tinggi radar → sangat kentara
#define TILT_FACTOR  0.30f

static uintptr_t g_base       = 0;
static bool      g_logged     = false;
static uintptr_t g_radarStruct = 0;  // cache pointer

static uintptr_t getRadarStruct() {
    if (!g_base) return 0;

    // Resolve PC-relative pointer dari binary
    // ldr r2,[pc,#0x74] di 0x43f618 → PC=0x43f61c → literal pool di 0x43f690
    // Karena Thumb2: PC = instruksi_addr + 4, tapi ldr literal = align-down-4(PC+offset)
    uint32_t rel   = *(uint32_t*)(g_base + OFF_PTR_TABLE);
    // PC untuk instruksi di 0x43f618 (2 byte) = 0x43f618 + 4 = 0x43f61c
    // ldr r2,[pc,#0x74]: target = (PC & ~3) + 0x74 = (0x43f61c & ~3) + 0x74
    //                           = 0x43f618 + 0x74 = 0x43f690  ✓ sesuai
    // Nilai di 0x43f690 = offset relatif dari PC 0x43f692 ke global var
    uintptr_t global_ptr_addr = (g_base + PC_AT_PTR) + rel;
    uintptr_t global_ptr      = *(uintptr_t*)global_ptr_addr;
    if (!global_ptr) return 0;

    uintptr_t radar_struct = *(uintptr_t*)(global_ptr + 0x284);
    return radar_struct;
}

typedef void (*tVoid)(void);
typedef void (*tVoidF)(float);

static tVoid  orig_DrawRadarMap = nullptr;
static tVoidF orig_DrawBlips    = nullptr;

// Simpan y1 asli untuk restore
static float g_savedY1 = 0.0f;
static bool  g_patched = false;

static void hk_DrawRadarMap() {
    uintptr_t rs = getRadarStruct();

    if (rs) {
        float* x1 = (float*)(rs + 32);
        float* y1 = (float*)(rs + 36);
        float* x2 = (float*)(rs + 40);
        float* y2 = (float*)(rs + 44);

        if (!g_logged) {
            _logf("[radarilt] radarStruct=0x%08X rect=(%.1f,%.1f,%.1f,%.1f)",
                  (unsigned)rs, *x1, *y1, *x2, *y2);
            g_logged = true;
        }

        float h    = *y2 - *y1;
        if (h > 0.0f && h < 2000.0f) {  // sanity check: nilai screen wajar
            g_savedY1  = *y1;
            *y1       += h * TILT_FACTOR;  // naikkan tepi atas
            g_patched  = true;
        }
    }

    orig_DrawRadarMap();

    // Restore segera setelah DrawRadarMap selesai
    if (g_patched && rs) {
        float* y1  = (float*)(rs + 36);
        *y1        = g_savedY1;
        g_patched  = false;
    }
}

static void hk_DrawBlips(float f) {
    // DrawBlips juga baca radar struct untuk TransformRPtoSS
    // Patch y1 juga di sini agar blip ikut ter-tilt
    uintptr_t rs = getRadarStruct();
    if (rs) {
        float* y1  = (float*)(rs + 36);
        float* y2  = (float*)(rs + 44);
        float  h   = *y2 - *y1;
        if (h > 0.0f && h < 2000.0f) {
            g_savedY1 = *y1;
            *y1      += h * TILT_FACTOR;
            g_patched = true;
        }
    }

    orig_DrawBlips(f);

    if (g_patched && rs) {
        float* y1 = (float*)(rs + 36);
        *y1       = g_savedY1;
        g_patched = false;
    }
}

MYMOD(brruham.libradarilt, libradarilt, 1.6, brruham)

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    _log("[radarilt] === v1.6 ===");
}

ON_MOD_LOAD() {
    _log("[radarilt] OnModLoad");

    uintptr_t base = 0;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps) { _log("ERROR: maps"); return; }
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libGTASA.so") && strstr(line, "r-xp")) {
                base = (uintptr_t)strtoul(line, nullptr, 16);
                break;
            }
        }
        fclose(maps);
    }
    if (!base) { _log("ERROR: base"); return; }
    g_base = base;
    _logf("[radarilt] base=0x%08X", (unsigned)base);

    // Log nilai pointer table untuk verifikasi
    uint32_t rel = *(uint32_t*)(base + OFF_PTR_TABLE);
    _logf("[radarilt] PTR_TABLE val=0x%08X → global=0x%08X",
          rel, (unsigned)((base + PC_AT_PTR) + rel));

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { _log("ERROR: dobby"); return; }
    auto dHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dHook) { _log("ERROR: sym"); return; }

    {
        void* t = (void*)(base + OFF_DrawRadarMap + 1);
        int r = dHook(t, (void*)hk_DrawRadarMap, (void**)&orig_DrawRadarMap);
        _logf("[radarilt] Hook DrawRadarMap ret=%d", r);
    }
    {
        void* t = (void*)(base + OFF_DrawBlips + 1);
        int r = dHook(t, (void*)hk_DrawBlips, (void**)&orig_DrawBlips);
        _logf("[radarilt] Hook DrawBlips ret=%d", r);
    }

    _logf("[radarilt] TILT_FACTOR=%.2f", TILT_FACTOR);
    _log("[radarilt] SELESAI");
    if (aml) aml->ShowToast(false, "[RadarTilt] v1.6");
}
