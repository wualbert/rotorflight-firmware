# Blackbox parameter journal: hardware bench handoff

The bench candidate includes the audit corrections in [Blackbox_Params_Audit.md](Blackbox_Params_Audit.md),
based on RF-4.6.x `53fd0637e`. It preserves Rotorflight 4.6 controller behavior; optional changes c1–c6 are excluded.
Software evidence, exact source revision, target binaries and SHA-256 hashes are in the validation package.
Use those local artifacts. The earlier prerelease assets are stale.

`blackbox_params` supports OFF, CHANGES and FULL; this candidate defaults to FULL. OFF preserves the stock header
and frame format. Blackbox 2.3.0 decodes the new logs, but does not interpret the parameter history.
The current repository's `tools/autotune/param_log.cjs` performs the history checks.

Docker validation is complete as recorded in the report. The following work requires the physical boards.
Hardware timing, storage throughput and stack headroom are acceptance measurements, not inferred software results.

## 1. Artifacts and physical setup

Use STM32G47X for the Matek G474 heli (`MTKS-MATEKG474HELI`) and STM32F7X2 for the NEXUS F7 (`RDMS-NEXUS_F7`).
Match the detected board before loading its MCU HEX. Keep the normal candidate, profiling candidate and stock
baseline together with the manifest. Do not add `FLASH_CONFIG_ERASE=yes`.

Remove main and tail blades. Disconnect motor power. Check that servos and linkages can move without obstruction.
Use the existing receiver and storage device so the test exercises the intended MSP/Lua transport and flash chip.
The optional profiling build requires the usual SWD debugger for its counters. It includes debug symbols and the
existing stack-check task; normal binaries omit that instrumentation.

## 2. Preserve and measure the stock configuration

For each board, download existing blackbox logs before erasing storage. Save `diff all` and `dump all`, including board
resources. Record `version`, `status`, `tasks`, logging settings and PID/filter denominators. Record a stock bench log
with the same configuration and the operations in section 4. Keep EEPROM-save stalls as a separate measurement.

Use identical logging fields, sample rate, receiver update rate and storage state for candidate comparisons.
Measure the stock arm-to-first-frame interval and frame interval distribution; no fixed historical estimate replaces
this board's baseline.

## 3. Flash and verify

In Rotorflight Configurator, enable advanced flashing options. Detect the board and retain its exact board selection
so its custom defaults are included. Disable full-chip erase, load the matching local HEX and flash it.
After reboot, verify `version` against the manifest, `defaults show`, `diff all`, `status` and `tasks`.
Check `blackbox_mode`, `blackbox_device`, `blackbox_params`, PID/filter denominators and board resources.

If settings were erased, restore the saved `diff all`; use `dump all` if board defaults or resources are missing.
Confirm the resulting configuration before arming. Do not substitute a different board target.

## 4. Functional bench sequence

First use the normal build in OFF, then CHANGES, then FULL. Record the original value of every setting changed and
restore it at the end. Download each run before deliberate storage-full or erase tests.

1. Arm and log for 10 seconds. Change yaw P by one using the receiver's Lua/MSP path and save.
2. Select PID profiles with both the receiver adjustment switch and the Lua page. Exercise rate-profile selection too.
3. Disarm, change yaw P back, save and re-arm within the configured grace period. This must remain one log.
4. Disarm and wait beyond the grace period. Change roll P by one, save, arm and record a new log.
5. Power-cycle and read back the saved values and active profiles. Record another log.
6. Repeat ordinary changes, saves and profile switches ten times. Exercise header-time changes, CMS and CLI entry
   during GRACE. CLI termination may leave a valid prefix without LOG_END.
7. Test SWITCH logging stop/restart. The stopped log must close and must not resume when the switch is turned on again.
8. On separate runs, exercise full storage, erase/restart and abrupt power loss. If serial logging is used, test 115200
   baud (FULL falls back to CHANGES) and the intended high baud rate.

Decode each file in the viewer repository:

```sh
node tools/autotune/param_log.cjs <downloaded-log.BBL>
```

Normal FULL logs require a complete header, valid header hash, zero CRC errors, zero unexplained sequence gaps and
successful self-test. Verify each setting's old/new values, profile index and operation source. Re-arm changes must
appear in the same log; between-log changes must precede the first frame of the next log. Copy/reset operations may
leave stored values pending until their loader runs; the viewer must not claim they already changed controller output.
OFF must have no parameter section, journal strings or additional slow fields.

For intentional loss, require conservative unknown/lost intervals where sequence/frame evidence shows missing records.
A write after the last complete frame can leave no evidence, so not every power loss can produce a trailing-loss flag.
A corrupted or truncated header must not establish a complete configuration. Investigate unexpected `u`, `y`, `L` or
unsent records in normal runs; interval recovery is not an exact operation timestamp.

## 5. Timing, storage and stack gates

Run the same cases with the matching profiling HEX. Attach the debugger using its matching ELF and source
`tools/validation/profiles.gdb`. Read the actual `SystemCoreClock`. Counter indices are HeaderTick, WriteHeader,
Running, AfterFrame, OpBegin, OpEnd and OperationSpan. The last includes the writer and loaders; measure EEPROM saves
separately. Reset counters while disarmed between scenarios. Do not halt the MCU during a timing capture.

The profile build exposes `bbpFlashfsDroppedBytes` and enables `USE_STACK_CHECK`; `status` reports stack allocation
and used bytes. Record these after the worst burst and header cases. Profile instrumentation adds work, so also compare
normal-build `tasks` and logged timing with stock. Linker free RAM is not a stack measurement.

Apply specification sections 3.12 and 6.6:

- No refused flash bytes in normal operation, no new late PID tasks, and no unexplained frame gaps.
- Ordinary operation maximum: 100 microseconds on G474 and 80 on F722. Record begin/end hooks and the total span.
- Idle journal work: at most 3 microseconds per logged iteration.
- Capture step: at most 10 microseconds on G474 and 8 on F722. Header call: at most 30 and 25, respectively.
- Measure the complete AfterFrame maximum including formatting and device work, plus scheduler/flush time. It must
  fit the configured PID deadline without introducing late tasks; a host estimate is not an acceptance result.
- FULL arm-to-first-frame increase: at most 160 milliseconds on Fireball and 40 on Gaui relative to their stock runs.
- Record stack high-water and remaining allocation under the largest configuration bursts. Any stack exhaustion fails.

Save logs, counters, task/status output and the exact binary hash with the bench results. A failing gate requires a fix
and repetition of its affected software and hardware checks. Flight validation starts only after these gates pass.

## 6. Rollback and handoff

Flash the matching stock baseline or official 4.6.0 with the correct board defaults. Restore the stock backup if needed.
A dump made by the candidate includes `set blackbox_params`; remove that unsupported line before restoring to stock.
Confirm resources, configuration, receiver, logging and version after rollback.

The software package does not authorize a flight or claim a hardware pass. Submit the PR with the recorded software
results and the completed physical bench measurements; keep missing measurements explicitly pending.
