# Blackbox parameter journal: software audit

The candidate includes the RF-4.6.x maintenance baseline `53fd0637e`. It preserves Rotorflight 4.6 controller behavior;
the optional controller changes c1–c6 remain outside this change. The previous prerelease `bbp-4.6.0-4318f222b`
does not contain these corrections. It is not the bench candidate.

Software validation is recorded in the generated validation report. Hardware execution time, flash behavior,
stack usage and flight behavior remain unmeasured. The candidate is for the controlled bench test, not flight approval.

| Finding | Correction | Software evidence / remaining measurement |
|---|---|---|
| A1: repeated header scan after yield | Return immediately on a formatter yield; precompute byte coverage; charge each covered byte to the 64-unit budget. | Header-yield regression, exact randomized header reconstruction, normal and shortened lines. Hardware header-call maximum remains a bench measurement. |
| A2: whole loader hash sweep at T0 | Verify captured shadow regions during bounded header ticks; invalidate inferred application markers after later changes. | At most 128 hashed bytes per tick and zero T0 hash bytes; stale-application regression; between-log simulator run. |
| A3: expensive drain | Compute CRC while formatting and write the event body through the storage bulk-string API. Preserve one event per logged iteration. | CRC/continuation round trips and burst/overflow tests. Maximum drain and combined PID/flush time remain bench measurements. |
| A4: read requests bracketed | Explicit tracked-write MSP allowlist, including the with-argument servo setter. | Static reachable-writer audit; negative tests remove three mutators or add a read command; read requests leave journal state unchanged. |
| A5: ISR reentry and unbounded walks | Reject journal entry from interrupt context; bound record/item walks and validate payload lengths. | Simulated ISR regression and bounded overflow tests. ISR writes are recovered as intervals by the scan; no exact ISR application marker is claimed. Unified STM32 targets disable SPI ExpressLRS. Physical SPI-ELRS transport is not exercised. |
| A6: insufficient storage reserve | Leave at least 256 bytes after each event. | Saturated-buffer regression and full-device tests. Real flash page-program latency remains a bench measurement. |
| A7: work while OFF | Skip inactive/OFF loader hashes and unused ESC fingerprints. | OFF hash regression; OFF/stock format comparison. |
| A8: adjustment cache thrashing | Per-range no-effect cache, invalidated on range reset. | Adjustment simulator scenarios; review confirms the stock setter is still called. |
| A9: oversized PG silently skipped | Assert every tracked PG fits the 1024-byte bitmap capacity. | Compile-time check in each hardware target and host variant. |
| A10: paused SWITCH log can resume during end hold | End PAUSED logs without the GRACE drain hold; update the budget specification. | SWITCH simulator regression and bounded-hold unit test. |
| A11: signed bitmap shift found during validation | Use unsigned 32-bit shifts throughout the journal. | Randomized configuration tests under UBSan, including word bit 31. |

The complete enabled host suite also required test-only maintenance: the current PID API and missing hardware stubs,
the current two-argument emulated FAT short-name API, and current expected filename suffixes. These changes do not
change flight firmware. No failing test was disabled. Pre-existing disabled tests remain identified in the test output.

The sanitizer configuration has two narrow ABI accommodations in `src/test/unit/sanitizer.ignore`: the existing typed
PG reset callback cast, and linker-collected registry descriptors that must remain contiguous. PG payloads, journal
buffers, formatter code and the remaining memory/undefined-behavior checks stay instrumented.

The reproducible Docker workflow and bench counter instructions are in `tools/validation/README.md`. The hardware
acceptance limits are in specification section 3.12 and testing guide section 5. Publishing a release or opening a PR
is not part of the software validation run.
