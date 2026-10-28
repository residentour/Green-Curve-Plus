// A populated VF entry that the driver silently pins at offset 0 no matter what is
// written (an idle/placeholder point below the operating range). Retrying it can
// never converge, so it is accepted instead of being retried and reported as a
// failure. (Ported from the 0.29 backend.)
static inline bool vf_offset_zero_readback_is_benign(int target, int actual,
    unsigned int liveKHz, unsigned int operatingMinKHz) {
    return target > 0 && actual == 0 && liveKHz > 0 && liveKHz < operatingMinKHz;
}

// Ported from the 0.29 backend: write in batch passes, read the offsets and the
// curve back after EVERY pass, feed the driver's readback into the next pass's
// base table, and fall back to single-point writes for whatever did not
// converge. The old version issued one batch and, when the driver accepted it
// (ret == 0) but only moved some points, reported failure without retrying --
// so "in-between" points were left unapplied.
static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses) {
    if (!targetOffsets || !pointMask) return false;

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    bool driverRefused[VF_NUM_POINTS] = {};
    int desiredCount = 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        if (!(g_app.vfMask[i / 8] & (1u << (i % 8)))) {
            debug_log("apply_curve_offsets_verified: point %d not in vfMask, skipping\n", i);
            continue;
        }
        desiredMask[i] = true;
        pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return true;

    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!setFunc) return false;

    unsigned char baseControl[0x4000] = {};
    if (backend->controlBufferSize > sizeof(baseControl)) return false;
    if (!nvapi_read_control_table(baseControl, sizeof(baseControl))) {
        debug_log("curve write: control table read failed, aborting before any write\n");
        return false;
    }

    bool anyWrite = false;
    bool batchFailed = false;
    bool readError = false;   // a readback itself failed (distinct from a rejected batch write)
    // A single point goes straight to the per-point path (Blackwell requires a
    // single-bit mask anyway).
    if (desiredCount <= 1) maxBatchPasses = 0;

    unsigned char batchBuf[0x4000];
    for (int pass = 0; pass < maxBatchPasses; pass++) {
        memcpy(batchBuf, baseControl, backend->controlBufferSize);
        unsigned char writeMask[32] = {};
        bool anyPendingWrite = false;
        int pointsInPass = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            unsigned int deltaOffset = backend->controlEntryBaseOffset
                + (unsigned int)i * backend->controlEntryStride
                + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(int) > backend->controlBufferSize) return false;
            int currentDelta = 0;
            memcpy(&currentDelta, &batchBuf[deltaOffset], sizeof(currentDelta));
            if (currentDelta == desiredOffsets[i]) { pendingMask[i] = false; continue; }
            memcpy(&batchBuf[deltaOffset], &desiredOffsets[i], sizeof(desiredOffsets[i]));
            writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
            anyPendingWrite = true;
            pointsInPass++;
        }
        if (!anyPendingWrite) break;

        memcpy(&batchBuf[backend->controlMaskOffset], writeMask, sizeof(writeMask));
        int setRet = setFunc(g_app.gpuHandle, batchBuf);
        debug_log("curve batch pass %d: points=%d ret=%d\n", pass + 1, pointsInPass, setRet);
        if (setRet != 0) { batchFailed = true; break; }
        anyWrite = true;

        bool readOk = false;
        for (int tryIdx = 0; tryIdx < 6; tryIdx++) {
            if (tryIdx > 0) Sleep(10);
            if (nvapi_read_offsets()) { readOk = true; break; }
        }
        if (!readOk) { batchFailed = true; readError = true; break; }
        if (!nvapi_read_curve()) { batchFailed = true; readError = true; break; }

        bool anyPending = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desiredMask[i]) continue;
            bool converged = (g_app.freqOffsets[i] == desiredOffsets[i]);
            if (!converged && vf_offset_zero_readback_is_benign(desiredOffsets[i],
                    g_app.freqOffsets[i], g_app.curve[i].freq_kHz, MIN_VISIBLE_FREQ_MHz * 1000u)) {
                if (!driverRefused[i]) {
                    driverRefused[i] = true;
                    debug_log("curve offset: driver pins point %d at 0 (wrote %dkHz); accepting as placeholder\n",
                        i, desiredOffsets[i]);
                }
                pendingMask[i] = false;
                continue;
            }
            pendingMask[i] = !converged;
            if (pendingMask[i]) {
                anyPending = true;
                debug_log("curve batch pass %d unconverged: ci=%d wrote=%dkHz readback=%dkHz\n",
                    pass + 1, i, desiredOffsets[i], g_app.freqOffsets[i]);
            }
            // The next pass starts from what the driver actually holds.
            unsigned int deltaOffset = backend->controlEntryBaseOffset
                + (unsigned int)i * backend->controlEntryStride
                + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(int) <= backend->controlBufferSize)
                memcpy(&baseControl[deltaOffset], &g_app.freqOffsets[i], sizeof(int));
        }
        if (!anyPending) break;
    }

    bool allOk = !batchFailed;
    bool hasPending = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (driverRefused[i]) { pendingMask[i] = false; continue; }
        if (g_app.freqOffsets[i] != desiredOffsets[i]) { pendingMask[i] = true; hasPending = true; }
        else pendingMask[i] = false;
    }

    if (hasPending) {
        // Everything the batch could not converge is written point by point.
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            bool pointOk = nvapi_set_point(i, desiredOffsets[i]);
            debug_log("curve fallback point %d target=%dkHz ok=%d (batch left readback=%dkHz)\n",
                i, desiredOffsets[i], pointOk ? 1 : 0, g_app.freqOffsets[i]);
            if (!pointOk) allOk = false; else anyWrite = true;
        }
        bool readOk = false;
        for (int tryIdx = 0; tryIdx < 6; tryIdx++) {
            if (tryIdx > 0) Sleep(10);
            if (nvapi_read_offsets()) { readOk = true; break; }
        }
        if (!readOk) { allOk = false; readError = true; }
    }

    if (anyWrite) {
        // The driver needs a moment to commit the new curve before the readback
        // that the editor/graph will display is trustworthy.
        bool settledOk = false;
        if (!read_live_curve_snapshot_settled(3, 10, &settledOk)) { allOk = false; readError = true; }
        rebuild_visible_map();
    }

    int unconverged = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i] || driverRefused[i]) continue;
        if (g_app.freqOffsets[i] != desiredOffsets[i]) {
            allOk = false;
            unconverged++;
            debug_log("curve offset unconverged (final): ci=%d wrote=%dkHz readback=%dkHz\n",
                i, desiredOffsets[i], g_app.freqOffsets[i]);
        }
    }
    // A rejected batch (Blackwell) is not a failure if the per-point fallback got
    // every point to its target; only unconverged points or failed readbacks are.
    if (unconverged == 0 && !readError) allOk = true;
    debug_log("apply_curve_offsets_verified: desired=%d unconverged=%d allOk=%d\n",
        desiredCount, unconverged, allOk ? 1 : 0);
    return allOk;
}

