#include <cstdio>
#include <cstring>
#define VF_NUM_POINTS 128
#define MIN_VISIBLE_FREQ_MHz 500
#define ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define StringCchPrintfA snprintf
struct Pt{unsigned freq_kHz;};
struct DesiredSettings{bool hasLock;bool hasCurvePoint[VF_NUM_POINTS];unsigned curvePointMHz[VF_NUM_POINTS];};
static struct{Pt curve[VF_NUM_POINTS];int numVisible;int visibleMap[VF_NUM_POINTS];void* hEditsMhz[VF_NUM_POINTS];}g_app;
static unsigned displayed_curve_mhz(unsigned k){return (k+500)/1000;}
static void graph_set_pending_mhz(int,unsigned){}
static void set_edit_value(void*,unsigned){}
static char g_applyClampNote[256];
static void debug_log(const char*,...){}
static void limit_requested_points_to_untouched_neighbors(DesiredSettings* full, bool updateUi) {
    if (updateUi) g_applyClampNote[0] = 0;
    if (!full || full->hasLock) return;
    int clamped = 0, firstCi = -1;
    unsigned int firstWanted = 0, firstLimit = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!full->hasCurvePoint[ci] || g_app.curve[ci].freq_kHz == 0) continue;
        unsigned int limit = 0;  // 0 = no untouched neighbor constrains this point
        const unsigned int curLive = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        for (int cj = ci + 1; cj < VF_NUM_POINTS; cj++) {
            if (g_app.curve[cj].freq_kHz == 0) continue;
            if (full->hasCurvePoint[cj]) break;  // governed by its own target from here on
            unsigned int liveJ = displayed_curve_mhz(g_app.curve[cj].freq_kHz);
            // Idle/placeholder entries (below the operating range, e.g. ~400 MHz) are not
            // part of the real curve and the driver does not lift them: never use them.
            if (liveJ < (unsigned int)MIN_VISIBLE_FREQ_MHz) continue;
            // A neighbor already below this point means the curve is not monotonic there;
            // it is not a limit this edit could respect.
            if (liveJ < curLive) continue;
            if (limit == 0 || liveJ < limit) limit = liveJ;
        }
        // Never limit a point below where it already is: the limiter may only hold a raise back.
        if (limit != 0 && limit < curLive) limit = curLive;
        if (limit == 0 || full->curvePointMHz[ci] <= limit) continue;
        if (clamped == 0) { firstCi = ci; firstWanted = full->curvePointMHz[ci]; firstLimit = limit; }
        clamped++;
        debug_log("limiter: point %d target %u -> %u MHz (point %d live=%u, lowest untouched neighbor to its right)\n",
            ci, full->curvePointMHz[ci] + 0u, limit, ci, curLive);
        full->curvePointMHz[ci] = limit;
        if (updateUi) {
            graph_set_pending_mhz(ci, limit);
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == ci) { set_edit_value(g_app.hEditsMhz[vi], limit); break; }
            }
        }
    }
    if (clamped > 0) {
        StringCchPrintfA(g_applyClampNote, ARRAY_COUNT(g_applyClampNote),
            " Limited %d point(s) so the untouched points to their right stay put (e.g. point %d: %u -> %u MHz). Raise those points too to go higher.",
            clamped, firstCi, firstWanted, firstLimit);
    }
}


static int fails=0;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("ok: %s\n",m);}while(0)
int main(){
  // real curve 60..97 rising to a flat 2953 tail, placeholder points 98..127 at ~405 MHz
  for(int i=0;i<VF_NUM_POINTS;i++){ g_app.curve[i].freq_kHz = (i<98)? (unsigned)((1500+i*15)*1000) : 405000u; }
  for(int i=90;i<98;i++) g_app.curve[i].freq_kHz=2953000u;           // flat tail
  g_app.numVisible=98; for(int i=0;i<98;i++) g_app.visibleMap[i]=i;
  DesiredSettings d={}; d.hasCurvePoint[95]=true; d.curvePointMHz[95]=3000;       // raise a tail point
  limit_requested_points_to_untouched_neighbors(&d,false);
  CHECK(d.curvePointMHz[95]==2953,"limit = real neighbor (2953), NOT the 405 MHz placeholder");
  DesiredSettings e={}; e.hasCurvePoint[97]=true; e.curvePointMHz[97]=3100;       // last real point, only placeholders to its right
  limit_requested_points_to_untouched_neighbors(&e,false);
  CHECK(e.curvePointMHz[97]==3100,"last real point is not limited by placeholders");
  DesiredSettings f={}; f.hasCurvePoint[50]=true; f.curvePointMHz[50]=(1500+50*15)+10; // small raise below neighbors
  limit_requested_points_to_untouched_neighbors(&f,false);
  CHECK(f.curvePointMHz[50]==(unsigned)(1500+50*15)+10,"raise below neighbor untouched");
  DesiredSettings g={}; g.hasCurvePoint[50]=true; g.curvePointMHz[50]=5000;
  limit_requested_points_to_untouched_neighbors(&g,false);
  CHECK(g.curvePointMHz[50]==(unsigned)(1500+51*15),"big raise limited to next real neighbor, never below current");
  // scenario from the user's log: point 96 raised to 2962, neighbors 97/98/99 at 2925/2932/2947
  for(int i=0;i<VF_NUM_POINTS;i++) g_app.curve[i].freq_kHz=(i<96)?(unsigned)((1500+i*10)*1000):405000u;
  g_app.curve[96].freq_kHz=2910000u; g_app.curve[97].freq_kHz=2925000u; g_app.curve[98].freq_kHz=2932000u; g_app.curve[99].freq_kHz=2947000u;
  for(int i=100;i<VF_NUM_POINTS;i++) g_app.curve[i].freq_kHz=2962000u;
  DesiredSettings h={}; h.hasCurvePoint[96]=true; h.curvePointMHz[96]=2962;
  limit_requested_points_to_untouched_neighbors(&h,false);
  CHECK(h.curvePointMHz[96]==2925,"user log case: 96 limited to 2925 so 97..99 are not lifted");
  printf(fails?"FAILED\n":"ALL PASSED\n"); return fails;}
