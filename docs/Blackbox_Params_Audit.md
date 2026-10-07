# Blackbox parameter journal: safety audit and open work (2026-10-06)

**Status: NOT READY FOR BENCH OR FLIGHT.** The pre-release `bbp-4.6.0-4318f222b` on GitHub has the PID-loop CPU spikes
below. Do not flash it until items A1-A3 are fixed and measured.

Branch `blackbox-param-journal` head `4318f222b` (worktree `~/exp/rotorflight-firmware/.wt/journal`). Audit = read-only
review of `git diff 118e912..HEAD -- src/main` by two independent reviewers (flight-behaviour equivalence; memory/timing
safety). All µs figures are estimates from disassembly / host timing, not measured on hardware.

## Verdict

- **Values: safe.** No hook writes a PG byte, a runtime controller field or changes a stock control-flow outcome
  (rc_adjustments, msp dispatch, EEPROM, CMS, acc calibration, loader markers checked line by line). The module only
  writes `pg->copy` (shadow), its own state and the log. INIT_CODE is only `-Os`, nothing discarded.
- **Memory: safe.** Buffer bounds, escaping, ring indices, aligned word diff (single LDR, all PGs 4-aligned), stack
  (~1.1 KB worst case in 2 KB), boot order, balanced op brackets, bounded LOG_END hold (state delay, not busy wait).
- **Timing: NOT safe yet.** CPU spikes in the PID task while armed.

## Fix list (do in this order)

| # | Sev | Problem | Where | Fix |
|---|---|---|---|---|
| A1 | HIGH | FULL header stalls the PID loop 0.15-0.8 ms per iteration after arming: `bbpHeaderNextLine()` returning 0 ("budget used, yield") is treated as an empty line, so up to 4 raw/boot coverage scans of up to 1 KB each run in one iteration; `bbpByteCovered` ~45-50 cycles/byte | `blackbox_params.c:1820-1834` (WriteHeader loop), `blackbox_params_format.c:510-517, 616-639, 899-921, 986-999` | `return false` when length==0; precompute coverage bitmap per PG at init (no udiv); count covered bytes 1 unit/byte |
| A2 | MED-HIGH | T0 `regionHash` of all idle loaders in one PID iteration: ~180 µs after a config save between flights (my P2 fix) | `blackbox_params.c:1246-1263, 1857-1863`; called from `blackbox.c:2237-2238` | spread over HeaderTick steps before T0, or replace hash with a change counter (`bbpChanges`) recorded at the idle apply |
| A3 | MED | Drain ~50-65 µs/logged frame while records wait (spec said 5-15): format + separate CRC pass + 131 × `blackboxWrite` | `blackbox_params.c:1434-1611` | CRC while formatting; bulk write; split format and write over two iterations; PAUSED runs AfterFrame every cycle (`blackbox.c:2255`) |
| A4 | MED | MSP bracket wraps read commands (BOXNAMES, BOXIDS, GET_SERVO_CONFIG, GET_MIXER_INPUT, GET_ADJUSTMENT_RANGE, RPM_FILTER_V2, VTXTABLE_*, unknown ids): 2 full compares (~55 µs) each | `msp.c:4092-4113`, `blackbox_params.c:1189-1216` | allowlist: bracket only In paths + write cases in WithArg (SET_SERVO_CONFIG, RESET_CONF) |
| A5 | MED latent | On SPI-ExpressLRS targets MSP runs in an ISR (`rx_sx1280.c:762` → `expresslrs_telemetry.c:285` → `mspFcProcessCommand`): ring corruption, ring walks with `!=` could spin. Not compiled in G47X/F7X2 builds | `blackbox_params.c:1476, 1906` | return early in MspBegin/OpBegin/Applied when `__get_IPSR() != 0`; bound both walks |
| A6 | LOW-MED | Event write reserve (len+3+192 of 512 B flashfs buffer) is thin: during a page program frames after an event can lose bytes | `blackbox_params.c:1598` | require free ≥ half buffer (FLASHFS_SUSPEND_THRESHOLD) or largest I-frame + event |
| A7 | LOW | OFF still hashes loader regions (~30 µs per in-flight profile switch); ESC-param FNV computed when unused | `blackbox_params.c:1298-1306`, `msp.c:3114` | early return when params == OFF |
| A8 | LOW | Each adjustment step = 2 full compares (~55 µs, RX task); single-entry `adjNoEffect` cache thrashes with two clamped ranges | `rc_adjustments.c:361-385` | small per-range cache; consider schedulerIgnoreTaskExecTime like changePidProfile |
| A9 | LOW | PG > 1024 B silently untracked (pidProfiles 912 B) | `blackbox_params.c:259` | STATIC_ASSERT |
| A10 | doc | Spec 3.12 budget and "one line per call" are out of date (now 4 lines/64 B); SWITCH mode: a PAUSED log in LOG_END hold can resume (stock ends it) | spec; `blackbox.c:1309, 2242-2252` | update spec; end hold on PAUSED |

## After fixing

1. Rebuild both targets clean (`make STM32G47X_clean STM32G47X ARM_SDK_DIR=$HOME/.local/arm-gnu/gcc-arm-none-eabi-9-2020-q2-update`, same for STM32F7X2); unit tests `cd src/test && make test_blackbox_params_unittest test_blackbox_params_short_unittest`.
2. SITL end-to-end: merge into `sitl-journal-test` (worktree `.wt/e2e`), `bash /tmp/e2e_final.sh`-style runs via `.wt/notes/e2e/vm_run.sh` and `analyze.cjs` (see `.wt/notes/e2e/NOTES.md`). VM: `arch -arm64 ~/.local/lima/bin/limactl shell rf-sitl`.
3. Add a DWT cycle counter (debug build) around WriteHeader, blackboxParamsRunning, AfterFrame and the op brackets; bench on hardware per `Blackbox_Params_Testing.md` section 5 with `tasks` and `debug_mode = SCHEDULER_DETERMINISM`. Gate: no new late PID tasks.
4. Replace the GitHub pre-release assets (`gh release` on wualbert/rotorflight-firmware, tag `bbp-4.6.0-<hash>`), or delete `bbp-4.6.0-4318f222b`.

## Not finished

- **Log-format compatibility audit** ("what breaks for other consumers") was stopped before reporting. Known so far:
  this viewer decodes it (old-style parsing identical on 18 SITL logs); ~460 extra header lines land in
  `paramHeader`; stock 4.6.0 refuses `save` of a fork `dump all` because of `set blackbox_params` (delete the line);
  blind window after arming +200-250 ms in FULL (CHANGES ~+20 ms); ~12-14 KB more per log. Still to check:
  blackbox_decode / PIDtoolbox / PID-Analyzer / Betaflight explorer, CSV export with 4 extra S fields, the
  tools/autotune health/datasets outputs on a FULL log vs OFF, analysis/gaui-x4 scripts.
- Viewer side (`rotorflight-blackbox`: param_log.cjs, param_semantics.cjs, parser/index/header_dialog/grapher edits) is
  uncommitted; wiring into the Tuning view is the peer session's job.
- Two stray Discord messages ("blackbox headers", "header") in Rotorflight #new-features need deleting by the user.
