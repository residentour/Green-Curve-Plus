#include <cstdio>
#include <cstdlib>
#include <cstring>
#define VF_NUM_POINTS 128
enum {GPU_FAMILY_BLACKWELL=5};
struct Pt{unsigned freq_kHz;};
static struct{Pt curve[VF_NUM_POINTS];int freqOffsets[VF_NUM_POINTS];int lockedCi;}g_app;
static void debug_log(const char*,...){}
static bool vf_curve_global_gpu_offset_supported(){return true;}
static bool is_gpu_offset_excluded_low_point(int pointIndex, int gpuOffsetMHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    if (g_app.curve[pointIndex].freq_kHz == 0) return false;

    (void)gpuOffsetMHz;

    int populatedCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (ci == pointIndex) return true;
        populatedCount++;
        if (populatedCount >= 70) return false;
    }
    return true;
}

static bool is_locked_tail_detection_point(int pointIndex) {
    return g_app.lockedCi >= 0 && pointIndex >= g_app.lockedCi;
}

static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut) {
    if (!vf_curve_global_gpu_offset_supported()) {
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
        return false;
    }

    static const int TOLERANCE_MHZ = 30;

    int candidateOffsets[VF_NUM_POINTS] = {};
    int candidateCount = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        int offsetMHz = g_app.freqOffsets[ci] / 1000;
        if (offsetMHz == 0) continue;

        bool seen = false;
        for (int i = 0; i < candidateCount; i++) {
            if (abs(candidateOffsets[i] - offsetMHz) <= TOLERANCE_MHZ) {
                seen = true;
                break;
            }
        }
        if (!seen && candidateCount < VF_NUM_POINTS) {
            candidateOffsets[candidateCount++] = offsetMHz;
        }
    }

    for (int candidateIndex = 0; candidateIndex < candidateCount; candidateIndex++) {
        int candidateMHz = candidateOffsets[candidateIndex];
        // The tolerance must stay well below the candidate itself. With a flat 30 MHz a
        // tiny single-point offset (e.g. 9 MHz) made every 0-offset point "match", and one
        // edited point was misdetected as a selective GPU offset over the whole curve.
        int effTolMHz = abs(candidateMHz) / 2;
        if (effTolMHz > TOLERANCE_MHZ) effTolMHz = TOLERANCE_MHZ;
        if (effTolMHz < 1) effTolMHz = 1;
        int toleranceKHz = effTolMHz * 1000;
        bool sawExcludedPoint = false;
        bool sawIncludedPoint = false;
        bool skippedLockedTail = false;
        int includedMatchSumKHz = 0;
        int includedMatchCount = 0;
        int includedConsideredCount = 0;
        int excludedViolations = 0;
        int excludedTotal = 0;
        int candidateTargetKHz = candidateMHz * 1000;

        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (g_app.curve[ci].freq_kHz == 0) continue;

            if (is_locked_tail_detection_point(ci)) {
                skippedLockedTail = true;
                continue;
            }

            bool excluded = is_gpu_offset_excluded_low_point(ci, candidateMHz);
            int actualOffsetKHz = g_app.freqOffsets[ci];
            if (excluded) {
                sawExcludedPoint = true;
                excludedTotal++;
                if (abs(actualOffsetKHz) > toleranceKHz) {
                    excludedViolations++;
                }
            } else {
                sawIncludedPoint = true;
                includedConsideredCount++;
                if (abs(actualOffsetKHz - candidateTargetKHz) <= toleranceKHz) {
                    includedMatchSumKHz += actualOffsetKHz;
                    includedMatchCount++;
                }
            }
        }

        if (!sawIncludedPoint || !sawExcludedPoint) continue;
        if (includedMatchCount == 0) continue;

        int minimumMatches = skippedLockedTail ? 2 : 3;
        if (includedMatchCount < minimumMatches) continue;
        if (includedMatchCount * 2 < includedConsideredCount) continue;
        if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) continue;

        if (sawIncludedPoint) {
            int matchedAverageKHz = includedMatchSumKHz / includedMatchCount;
            int detectedMHz = (matchedAverageKHz >= 0 ? (matchedAverageKHz + 500) : (matchedAverageKHz - 500)) / 1000;
            if (detectedMHz == 0) continue;  // "selective offset of 0 MHz" is no offset at all
            debug_log("detect_live_selective: found selective offset=%d MHz (candidate=%d MHz, matches=%d/%d, skippedTail=%d)\n",
                detectedMHz, candidateMHz, includedMatchCount, includedConsideredCount, skippedLockedTail ? 1 : 0);
            if (gpuOffsetMHzOut) *gpuOffsetMHzOut = detectedMHz;
            return true;
        }
    }

    debug_log("detect_live_selective: no selective offset pattern found (candidates=%d)\n", candidateCount);
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    return false;
}


static int fails=0;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("ok: %s\n",m);}while(0)
static void world(){memset(&g_app,0,sizeof g_app);g_app.lockedCi=-1;for(int i=0;i<100;i++)g_app.curve[i].freq_kHz=(1500+i*10)*1000;}
int main(){
  int mhz=-1;
  world(); g_app.freqOffsets[96]=9000;                      // the user's case: ONE point +9 MHz
  CHECK(!detect_live_selective_gpu_offset_state(&mhz),"single point +9 MHz is NOT a selective GPU offset");
  world(); g_app.freqOffsets[96]=57000;                     // one point +57 MHz
  CHECK(!detect_live_selective_gpu_offset_state(&mhz),"single point +57 MHz is NOT a selective GPU offset");
  world(); for(int i=0;i<100;i++){ int pop=i+1; g_app.freqOffsets[i]=(pop>70)?150000:0; }
  bool f=detect_live_selective_gpu_offset_state(&mhz);
  CHECK(f && mhz==150,"real selective +150 MHz (points after the 70th) is still detected");
  world(); for(int i=0;i<100;i++){ int pop=i+1; g_app.freqOffsets[i]=(pop>70)?15000:0; }
  f=detect_live_selective_gpu_offset_state(&mhz);
  CHECK(f && mhz==15,"real selective +15 MHz is still detected");
  printf(fails?"FAILED\n":"ALL PASSED\n"); return fails;}
