# Blackbox parameter journal: flashing and testing on a helicopter

This fork (branch `blackbox-param-journal`, on Rotorflight 4.6.0 `118e912`) makes every blackbox log carry the exact
parameter values that produced each frame, as ArduPilot (PARM) and PX4 (ULog parameter messages) do:

- **Header snapshot.** At the start of each log: every tracked setting (30 parameter groups, all 6 PID profiles and all
  6 rate profiles), the profile indices, boot-time values that differ from the stored ones, and a line count + hash.
- **Per frame.** Four new slow-frame fields: `pidProfile`, `rateProfile` (0-5), `armed`, `paramSeq`.
- **Change journal.** Every change while a log is open (MSP / Lua / Configurator writes, adjustments, profile switches,
  EEPROM load/save, CMS, trims) and every change between two logs of one boot, as event-101 (`CUSTOM_STRING`) records
  with the exact frame and loop tick, old and new value, a sequence number and a CRC. Lost records are always detected.
- **No change to flight behaviour.** Values are applied exactly when and how stock 4.6.0 applies them.

Setting: `blackbox_params = OFF | CHANGES | FULL` (default `FULL`). `OFF` writes logs in the stock 4.6.0 format.
Design: [`Blackbox_Params_Spec.md`](Blackbox_Params_Spec.md).

Status: unit tests (32), static audit of every writer, and SITL end-to-end tests pass (47,476 per-frame value checks
against ground truth, 0 mismatches). **Not yet run on hardware.** Do the bench test below before any flight.

## 1. Build

```sh
git switch blackbox-param-journal
make arm_sdk_install          # once
make STM32G47X                # Matek G474 heli (MTKS-MATEKG474HELI) -> obj/rotorflight_4.6.0_STM32G47X.hex
make STM32F7X2                # RDMS NEXUS_F7                       -> obj/rotorflight_4.6.0_STM32F7X2.hex
```

Do not add `FLASH_CONFIG_ERASE=yes` (that marker makes the board erase its configuration). Release hex files are
attached to the GitHub pre-release of this branch.

## 2. Back up (each helicopter, on its current firmware)

1. Download the blackbox flash (Configurator Blackbox tab, or `tools/flash_read.py` in the viewer repository).
2. CLI tab: `diff all` -> Save to File. Then `dump all` -> Save to File (the fallback: it has every resource line).
3. Write down `tasks` and `status` output (CPU load, PID loop time) to compare after the flash.

## 3. Flash with Rotorflight Configurator 2.3.0 (keeps the configuration)

1. Options tab: enable **Show advanced firmware flashing options** (otherwise "Full chip erase" is forced on).
2. Connect, open Firmware Flasher, click **Detect**. The board list must show `MTKS-MATEKG474HELI` (Fireball) or
   `RDMS-NEXUS_F7` (Gaui). Be online: the Configurator downloads the board configuration and inserts it into the hex.
3. **Untick Full chip erase.**
4. **Load Firmware [Local]** -> the matching hex (`STM32G47X` for the Matek, `STM32F7X2` for the NEXUS). Do not change
   the board list after this.
5. **Flash Firmware.**

If the configuration was wiped anyway (chip erase left on): the board boots on its board defaults; paste the `diff all`
backup and `save`. If the board defaults are missing too (no board was selected), paste `dump all` and `save`.

## 4. Check after the flash (CLI)

| Command | Expected |
|---|---|
| `version` | `Rotorflight 4.6.0 (<hash of this branch>)` |
| `defaults show` | starts with `## Rotorflight Custom Defaults` and the right board |
| `get blackbox_params` | `FULL` |
| `diff all` | the backup, apart from the version line |
| `get blackbox_mode`, `get blackbox_device` | as before (ARMED, SPIFLASH) |
| `tasks` | PID loop and total CPU close to the numbers from step 2.3 |

## 5. Bench test (blades off, motor unpowered or held at idle)

Erase the blackbox flash first, then do each step and write down the time from your watch or the radio:

1. Arm. Wait 10 s.
2. While armed, with the Lua script: change yaw P by +1 and **Save**.
3. While armed: switch PID profile with your profile switch; then select another PID profile on a Lua page.
4. Disarm. Within 2 s, change yaw P by -1 with the Lua and Save. Re-arm within 1 s of the save. Wait 10 s. Disarm.
5. Wait 10 s (the log ends 5 s after the disarm). Change roll P by +1, Save, arm again (a new log). Wait 10 s. Disarm.
6. Power cycle. Arm, wait 10 s, disarm (a log after a reboot).
7. Optional: pull the battery while armed and recording (the last records must show as lost, not as missing).

Download the log and decode it with the viewer repository (`rotorflight-blackbox`):

```sh
node tools/autotune/param_log.cjs RTFL_BLACKBOX_LOG_<...>.BBL
```

Pass criteria, for each log:

- `Param log 1 FULL, header complete`, `0 gaps`, `0 CRC errors`, `self-test ok`.
- Every save of steps 2-5 is a `C` record with source `m.<command>` and the right key, old and new value
  (`p<k>.yaw_p_gain=81<80` and so on; `k` is the 0-based PID profile).
- Step 3: the profile changes show in the epochs ("PID profile N") with no `unknown`.
- Step 4: one log with two arms; the change in the grace period is a `C` record before the re-arm.
- Step 5: the new log starts with `v` records (changes between logs) and its values are exact from frame 0.
- Step 7 (if done): the last frames are `unknown, lost`; nothing is silently wrong.
- No `u` or `y` records (they mean a write that no hook saw, or a buffer overflow).

Timing gates before the first flight (spec section 6.6):

- `tasks`: PID loop and gyro task times within noise of stock; no new late tasks.
- In the viewer, the frame interval distribution matches a stock log of the same setup (apart from the known 26 ms
  (F7) / 71 ms (G474) stall 0.5 s after a disarm, which stock 4.6.0 also has: the delayed EEPROM save).
- Arm to first frame: stock is about 230 ms; FULL adds about 200-250 ms (no data in that window). If that matters,
  use `set blackbox_params = CHANGES` (about +20 ms; values that never change are then only in the classic header).

## 6. First flight

Hover only, normal profile, then land, disarm and wait 10 s. Decode the log as in section 5. Compare a stock log of
the same pack for loop timing and for missing frames.

## 7. Roll back

- Keeping the settings: build `release/4.6.0` (`git switch --detach release/4.6.0 && make STM32G47X`) and flash it
  as in section 3. Official 4.6.0 ignores the extra configuration byte.
- Official route: Load Firmware [Online], 4.6.0, board selected. This always erases the configuration; restore the
  `diff all` backup. A `diff all` or `dump all` made on this fork contains `set blackbox_params = ...` (always in
  `dump all`): delete that line first, or stock 4.6.0 refuses to `save` ("INVALID NAME").

## 8. Known limits

- A UART blackbox logger below 1 Mbaud gets `CHANGES` instead of `FULL` (the header would take seconds).
- A write while a log is open costs an estimated 70-90 µs in the MSP task on the G474 (measure with `tasks`).
- More than about 1 KB of changes between two logs overflows the record buffer before the log starts; those groups
  are then "unknown" until their resync record, a few frames later.
- ESC-internal settings, radio settings and mechanics are not in the log.
