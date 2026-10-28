# Green Curve Plus 0.12 - apply fixes
1. Removed the pre-apply "zero every VF offset" reset (gpu_backend.cpp). Untouched points are no longer wiped; this was the cause of unapplied in-between frequencies and transient odd graphs.
2. Base frequency = live - offset (same as 0.29) instead of vf_tuple_base (off ~15 MHz on Blackwell).
3. apply_curve_offsets_verified (main.cpp) rewritten from 0.29: multi-pass batch, readback after every pass, per-point fallback for unconverged points, vfMask respected, driver-pinned placeholder points accepted.
4. Fail-closed rollback to the pre-apply curve when verification fails.
tests/: mock-driver logic test (g++ -I tests tests/test_curve_writer.cpp). Not tested on real hardware.
5. Graph editing (drag, Shift+A, Ctrl+Z) no longer writes into g_app.curve (the hardware mirror). Edits are kept in g_app.graphPendingMHz and flagged guiCurvePointExplicit, so Apply sends them and a later readback no longer snaps them back.
6. detect_locked_tail_from_curve no longer discovers a lock from a flat live tail (single-point edits no longer drag the whole tail).
7. Post-apply collateral report (message + log): lists unrequested points whose frequency changed and whether the offset changed (offset write) or not (driver re-shaped the curve).
8. Apply decides which points the user edited by comparing edit boxes with the values they were filled with (guiEditBaselineMHz), not with the drifting live curve; result message states how many curve points were requested.
9. GUI Apply limits a requested point to the lowest current frequency of the untouched points to its right (driver would otherwise lift them), reports the limit, and updates the edit box/graph.
10. Limiter ignores placeholder points (<500 MHz) and never limits below the point's current value (fixes a bug where a point was written to 405 MHz). tests/test_limiter.cpp covers it.
11. The untouched-neighbor limiter also runs inside apply_desired_settings on the fresh readback (no UI calls from the worker thread) and logs 'limiter:' lines. Verified with the numbers from a real debug log (point 96 -> 2962 lifted 97..99).
12. Selective GPU offset detection no longer treats a single edited point as a whole-curve offset (tolerance now <= half the candidate; a 0 MHz result is rejected). Debug log gets after-apply neighbor dump + a +1.5 s late-collateral check. tests/test_selective.cpp.
13. ROOT CAUSE: async_apply_thread zeroed every curve offset + GPU/mem offset + power limit before EVERY Apply, while the GUI only sends changed values. Removed (ui_main.cpp). Reset button unchanged.
