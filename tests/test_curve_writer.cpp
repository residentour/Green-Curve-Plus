// Logic test for apply_curve_offsets_verified() against a mock NVAPI driver.
// The function body is #included verbatim from new_curve_writer.cpp, so this
// exercises exactly the code that ships in main.cpp.
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>

#define VF_NUM_POINTS 128
#define MIN_VISIBLE_FREQ_MHz 500
static void Sleep(unsigned) {}

struct VfBackendSpec {
    bool writeSupported;
    unsigned setControlId;
    unsigned controlBufferSize;
    unsigned controlMaskOffset;
    unsigned controlEntryBaseOffset;
    unsigned controlEntryStride;
    unsigned controlEntryDeltaOffset;
};
struct CurvePt { unsigned freq_kHz; unsigned volt_uV; };
static VfBackendSpec g_backend = {true, 1, 0x2000, 0x10, 0x40, 0x1C, 0x14};
static struct {
    const VfBackendSpec* vfBackend;
    CurvePt curve[VF_NUM_POINTS];
    int freqOffsets[VF_NUM_POINTS];
    unsigned char vfMask[32];
    void* gpuHandle;
} g_app;

typedef int (*NvApiFunc)(void*, void*);

// ---------------- mock driver ----------------
enum Mode { HONEST, BATCH_REJECT, BATCH_PARTIAL_OK, PIN_ZERO_PLACEHOLDER };
static Mode g_mode = HONEST;
static int g_drvOffsets[VF_NUM_POINTS];       // what the "hardware" holds
static unsigned g_baseKHz[VF_NUM_POINTS];     // stock curve
static int g_batchCalls = 0, g_singleCalls = 0;
static int g_ignoreFirstPoint = -1;           // BATCH_PARTIAL_OK: silently skip this point in batch writes

static void debug_log(const char*, ...) {}
static int clamp_freq_delta_khz(int v) { if (v > 1000000) return 1000000; if (v < -1000000) return -1000000; return v; }
static void rebuild_visible_map() {}

static unsigned entryDelta(int i) { return g_backend.controlEntryBaseOffset + i * g_backend.controlEntryStride + g_backend.controlEntryDeltaOffset; }

static int mock_set(void*, void* bufv) {
    unsigned char* buf = (unsigned char*)bufv;
    unsigned char* mask = &buf[g_backend.controlMaskOffset];
    int bits = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) if (mask[i / 8] & (1u << (i % 8))) bits++;
    bool single = (bits == 1);
    if (single) g_singleCalls++; else g_batchCalls++;
    if (!single && g_mode == BATCH_REJECT) return -5;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!(mask[i / 8] & (1u << (i % 8)))) continue;
        int v; memcpy(&v, &buf[entryDelta(i)], 4);
        if (!single && g_mode == BATCH_PARTIAL_OK && i == g_ignoreFirstPoint) continue;  // ret 0, but not moved
        if (g_mode == PIN_ZERO_PLACEHOLDER && g_baseKHz[i] < 500000) continue;           // driver pins it at 0
        g_drvOffsets[i] = v;
    }
    return 0;
}
static void* nvapi_qi(unsigned) { return (void*)mock_set; }
static bool nvapi_read_control_table(unsigned char* buf, size_t) {
    memset(buf, 0, g_backend.controlBufferSize);
    for (int i = 0; i < VF_NUM_POINTS; i++) memcpy(&buf[entryDelta(i)], &g_drvOffsets[i], 4);
    return true;
}
static bool nvapi_read_offsets() { for (int i = 0; i < VF_NUM_POINTS; i++) g_app.freqOffsets[i] = g_drvOffsets[i]; return true; }
static bool nvapi_read_curve() {
    for (int i = 0; i < VF_NUM_POINTS; i++) g_app.curve[i].freq_kHz = g_baseKHz[i] ? (unsigned)((int)g_baseKHz[i] + g_drvOffsets[i]) : 0;
    return true;
}
static bool read_live_curve_snapshot_settled(int, unsigned, bool* ok) { nvapi_read_curve(); nvapi_read_offsets(); if (ok) *ok = true; return true; }
static bool nvapi_set_point(int i, int delta) {
    if (!(g_app.vfMask[i / 8] & (1u << (i % 8)))) return false;
    unsigned char buf[0x2000];
    nvapi_read_control_table(buf, sizeof buf);
    memset(&buf[g_backend.controlMaskOffset], 0, 32);
    buf[g_backend.controlMaskOffset + i / 8] = (unsigned char)(1u << (i % 8));
    memcpy(&buf[entryDelta(i)], &delta, 4);
    return mock_set(nullptr, buf) == 0;
}

#include "new_curve_writer.cpp"

// ---------------- harness ----------------
static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); g_fail++; } else printf("  ok:   %s\n", msg); } while (0)

static void reset_world(Mode m) {
    g_mode = m; g_batchCalls = g_singleCalls = 0; g_ignoreFirstPoint = -1;
    memset(&g_app, 0, sizeof g_app);
    g_app.vfBackend = &g_backend;
    memset(g_app.vfMask, 0xFF, 16);
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        g_baseKHz[i] = (i < 10) ? 300000u : 600000u + i * 15000u;   // first 10 = placeholder entries (<500 MHz)
        g_drvOffsets[i] = 0;
    }
    // Pre-existing custom offsets on "in-between" points that must survive.
    g_drvOffsets[50] = 75000; g_drvOffsets[51] = 75000; g_drvOffsets[52] = 75000;
    nvapi_read_curve(); nvapi_read_offsets();
}

int main() {
    printf("1) honest driver, 3 points edited, untouched points keep their offsets\n");
    reset_world(HONEST);
    { int t[VF_NUM_POINTS] = {}; bool m[VF_NUM_POINTS] = {}; t[60] = 90000; t[61] = 90000; t[62] = 120000; m[60] = m[61] = m[62] = true;
      bool ok = apply_curve_offsets_verified(t, m, 2);
      CHECK(ok, "returns true");
      CHECK(g_drvOffsets[60] == 90000 && g_drvOffsets[62] == 120000, "edited points written");
      CHECK(g_drvOffsets[50] == 75000 && g_drvOffsets[52] == 75000, "untouched in-between points NOT wiped"); }

    printf("2) driver accepts batch (ret 0) but silently skips one point -> per-point fallback must fix it\n");
    reset_world(BATCH_PARTIAL_OK); g_ignoreFirstPoint = 61;
    { int t[VF_NUM_POINTS] = {}; bool m[VF_NUM_POINTS] = {}; t[60] = 90000; t[61] = 90000; t[62] = 120000; m[60] = m[61] = m[62] = true;
      bool ok = apply_curve_offsets_verified(t, m, 2);
      CHECK(ok, "returns true after fallback");
      CHECK(g_drvOffsets[61] == 90000, "skipped point 61 was applied by the fallback");
      CHECK(g_singleCalls >= 1, "single-point write was used"); }

    printf("3) driver rejects batch writes (Blackwell style) -> every point via single writes\n");
    reset_world(BATCH_REJECT);
    { int t[VF_NUM_POINTS] = {}; bool m[VF_NUM_POINTS] = {}; for (int i = 70; i < 76; i++) { t[i] = 60000 + (i - 70) * 1000; m[i] = true; }
      bool ok = apply_curve_offsets_verified(t, m, 2);
      CHECK(ok, "returns true");
      bool all = true; for (int i = 70; i < 76; i++) if (g_drvOffsets[i] != t[i]) all = false;
      CHECK(all, "all six points applied");
      CHECK(g_singleCalls == 6, "exactly one single write per point (no wasteful retries)"); }

    printf("4) placeholder point the driver pins at 0 is accepted, not reported as failure\n");
    reset_world(PIN_ZERO_PLACEHOLDER);
    { int t[VF_NUM_POINTS] = {}; bool m[VF_NUM_POINTS] = {}; t[3] = 50000; t[60] = 90000; m[3] = m[60] = true;
      bool ok = apply_curve_offsets_verified(t, m, 2);
      CHECK(ok, "returns true despite pinned placeholder");
      CHECK(g_drvOffsets[60] == 90000, "real point applied"); }

    printf("5) point outside vfMask is skipped\n");
    reset_world(HONEST); g_app.vfMask[60 / 8] &= (unsigned char)~(1u << (60 % 8));
    { int t[VF_NUM_POINTS] = {}; bool m[VF_NUM_POINTS] = {}; t[60] = 90000; t[61] = 90000; m[60] = m[61] = true;
      apply_curve_offsets_verified(t, m, 2);
      CHECK(g_drvOffsets[60] == 0, "masked-out point untouched");
      CHECK(g_drvOffsets[61] == 90000, "other point applied"); }

    printf("%s (%d failure(s))\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
