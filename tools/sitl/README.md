# Rotorflight SITL test harness (Linux)

Runs the Rotorflight 4.6.0 firmware as a Linux process (target `SITL`) with a
simulated IMU and receiver, drives it over MSP and records its blackbox log.
There is no physics simulation: the craft is level and still unless
`sim_feed.py` is told to inject gyro rates.

Tested on Ubuntu 24.04 aarch64 (gcc 13.3, Python 3.12) in a Lima VM on an
Apple M1 Mac. Upstream CI does not build SITL; the commits on this branch
that start with "SITL:" make it build and run on 4.6.0. They change only
SITL files (`make/mcu/SITL.mk`, `src/main/target/SITL/*`,
`src/main/drivers/serial_tcp.[ch]`), so the ARM targets are unchanged.

## Build

```sh
make SITL ARM_SDK_DIR=/usr ARM_SDK_PREFIX=          # -> obj/main/rotorflight_SITL.elf
```

`make/tools.mk` requires an ARM toolchain for every target. `ARM_SDK_DIR=/usr`
satisfies that check and `ARM_SDK_PREFIX=` (empty) selects the host `gcc`.
A clean build takes about 80 s in the VM (virtiofs, `-j6`), a rebuild after a
change to one file about 9 s (LTO link).

From macOS with the Lima VM `rf-sitl` (the firmware tree is mounted at the
same path):

```sh
limactl shell rf-sitl -- bash -lc 'cd ~/exp/rotorflight-firmware && tools/sitl/run_all.sh -n regrace'
```

## Run a scenario

```sh
tools/sitl/run_all.sh [-n NAME] [-s SCENARIO] [-d DENOM] [-- SCENARIO ARGS...]
```

`run_all.sh` builds (incremental), boots a fresh `eeprom.bin` (the first boot
writes the defaults and exits), starts `sim_feed.py`, applies
`configure.py` (saves and reboots: the SITL process exits), starts the SITL
again with `capture.py`, runs `scenario_<SCENARIO>.py`, waits 1 s and stops
everything. It takes about 25 s. Output in `obj/sitl_runs/NAME/`:

| File | Content |
|---|---|
| `blackbox.bbl` | raw blackbox stream of UART2, as an OpenLog would write it |
| `blackbox.bbl.chunks.csv` | host CLOCK_MONOTONIC, offset and size of each received TCP chunk |
| `timeline.json` | scenario steps: host CLOCK_MONOTONIC and SITL time (`t_sitl_us`) |
| `configure.json`, `capture.json` | configuration that was applied, capture statistics |
| `sitl.log`, `boot0.log`, `boot1.log`, `feed.log` | process output |
| `eeprom.bin` | the configuration |

`t_sitl_us` is host time minus the `CLOCK_MONOTONIC start` that the SITL
prints at boot. The SITL clock runs at simRate (about 1.0000 with
`sim_feed.py`), so it matches the `time` field of the log to some ms.

## Interfaces

| Port | Direction | Content |
|---|---|---|
| UDP 9003 | to SITL | `fdm_packet` (target.h): time, gyro rad/s, accel m/s^2 (NED body), quaternion. Sets the fake gyro and accelerometer and the attitude. Also sets simRate (sim time / real time, over >= 200 ms) |
| UDP 9002 | from SITL | `servo_packet`: 4 motor outputs 0..1, one per FDM packet |
| UDP 9004 | to SITL | `rc_packet`: time and 16 channels in us, to the MSP receiver (`FEATURE_RX_MSP`, the SITL default). Same layout as the Betaflight SITL |
| UDP 9010 | to `sim_feed.py` | JSON control: `{"rc": {"5": 1900}}`, `{"gyro_dps": [r, p, y]}`, `{"gyro_sine": {...}}`, `{"get": true}` |
| TCP 5760+N | both | UART N. The port opens when the firmware opens the UART: UART1 at boot, the blackbox UART when a log starts. One client per port |

The SITL keeps its configuration in `eeprom.bin` in the working directory. A
reboot (CLI `exit`/`reboot`, `MSP_REBOOT`) stops the process.

## Configuration (`configure.py`)

Applied over MSP to a fresh `eeprom.bin`, then saved:

- accelerometer calibration (`MSP_ACC_CALIBRATION`, needs the level, still feed)
- mode range 0: ARM on AUX1 (aux channel 0 = RC channel index 5), 1700..2100 us
- UART1 (TCP 5761): MSP. UART2 (TCP 5762): BLACKBOX at 2 Mbaud (1 Mbaud or more
  removes the header rate limit of `blackbox_io.c`; TCP has no baud rate)
- blackbox device SERIAL, mode ARMED, `denom` 2 (`-d`), grace period 5 s

The defaults give the rest: rcmap `AETRC123` (roll, pitch, throttle, yaw,
collective, AUX1...), PID profile 1, roll P 50, no governor, no motors in the
mixer. Gyro 8 kHz (the fake gyro gets the default rate of `gyroSetSampleRate`),
`pid_process_denom` 8: PID loop 1 kHz, blackbox 500 Hz with `denom` 2.

## Arming blockers found and fixed

| Symptom | Cause | Fix |
|---|---|---|
| No MSP reply; sometimes a UART never accepts a connection | dyad (TCP) is not thread safe; the main loop called it for each byte and to open ports | dyad only in the TCP thread (`serial_tcp.c`, `target.c`) |
| `BOOT_GRACE_TIME` never clears; GYRO, PID and RX tasks do not run | SITL clock: simRate was 1e-6 after start-up (delay() slept for hours), or about 100 when packets came in bursts (uptime 85 s after 6 s) | simRate over >= 200 ms, one locked clock (`target.c`) |
| `CALIBRATING` stays set | `gyro.c` skips the calibration of a fake gyro, but 4.6 needs `calibration.cycles > 0` | the SITL marks a skipped calibration as done (`target.c`); the proper fix is in `gyro.c` |
| `ACC_CALIBRATION` (`NO_ACC_CAL`) | a fresh `eeprom.bin` has no accelerometer calibration | `configure.py` |
| `RX_FAILSAFE` | no RC input except `MSP_SET_RAW_RC` | RC on UDP 9004, `sim_feed.py` |
| `ANGLE`, `CALIBRATING` without IMU data | no FDM packets | `sim_feed.py` |
| arm switch does nothing | no ARM mode range | `configure.py` |
| `ACC_CALIBRATION` after the configuration reboot (1 run in 4) | `configure.py` read the flags before the first `updateArmingStatus()` (RX task), so a fresh `eeprom.bin` showed no `ACC_CALIBRATION` and the calibration was skipped | `configure.py` waits until `BOOT_GRACE_TIME` clears (that call clears it) |
| `LOAD` comes and goes for seconds (61 % of 175 status reads in 45 s idle with the Mac busy, load average 7) | max real-time load > 75 % (`fc/core.c`): a host stall of 0.1 ms or more in the gyro/PID section. The scheduler only zeroes the CPU and system loads for `SIMULATOR_BUILD`. An arm switch during `LOAD` sets `ARM_SWITCH` | `scenario_regrace.py` arms through `arm()`: switch low and try again; the timeline has the failed attempts |

Transient after boot: `BOOT_GRACE_TIME` (3 s), `BST` (1 s RX recovery after
the first valid RC frame). `scenario_regrace.py` waits until only
`ARM_SWITCH` may be set. Transient at any time when the host is busy: `LOAD`.

## Scenario `regrace`

`scenario_regrace.py`: roll stick 1600 us (constant rate error, so the logged
`axisP[0]` is the roll P gain times the error), arm, wait 3 s, set roll P to
twice its value with `MSP_SET_PID_TUNING` while armed (and read it back), wait
2 s, disarm, re-arm 1 s later (inside the 5 s grace period), wait 3 s, disarm,
wait 8 s for `LOG_END`.

Result on stock 4.6.0 (with the SITL commits): one log, one header (roll P
50), `axisP[0]` / (`setpoint[0]` - `gyroADC[0]`) goes from 0.345 to 0.655
(10 -> 19 at an error of 29 deg/s) at the MSP write, with no event; the
re-arm continues the same log (FLIGHT_MODE arm bit 0 -> 1, no new header);
then DISARM and LOG_END.

## Scenario `journal`

`scenario_journal.py` (with `blackbox_params`, docs/Blackbox_Params_Spec.md
6.3): roll and yaw sticks off centre, arm, roll P x2 and `--toggles` more
roll P writes while armed, PID profile 2 and back (`MSP_SELECT_SETTING`),
disarm, yaw P in the grace period, re-arm within 1 s, disarm,
`MSP_EEPROM_WRITE` in the grace period, `LOG_END`. `--between`: writes with no
log open, then a second log (`v` records). `--log2-idle`: no writes in the
second log. `--kill-ms D`: `kill -9` of the SITL D ms after a write. Each
write is in `timeline.json` with `write`, `truth` ({journal key: [old, new]}),
the host time before and after (`t_sitl_send_us`, `t_sitl_reply_us`), the
`MSP_STATUS` before it and the read-back.

`run_all.sh -c LINE` runs CLI lines in `configure.py` (then `save`), for
example `-c 'set blackbox_params = OFF'`. `-e ELF` uses another binary.
`pid_process_denom` below 8 needs a lower fake gyro rate: the SITL has
`MAX_PID_PROCESS_SPEED` 1000 (`target/common_pre.h`) and an 8 kHz fake gyro,
so `validateAndFixGyroConfig()` sets 8 again.

## Limitations

- Loop timing: the main loop sleeps 50 us after each `scheduler()` call (about
  80 us in the VM). The 8 kHz gyro task cannot keep up; the PID loop runs at
  870..990 Hz instead of 1 kHz (measured from the logs of 8 runs; slower when
  the host is busy). In the
  500 Hz logs, 90..98 % of the frame intervals are 1.9..2.5 ms, 0.5..3 % are
  4..10 ms and 1..6 per run are 10..44 ms (host load matters). These are late
  PID loops, not lost frames: `loopIteration` has no gaps.
- The fake gyro gets new data only with each FDM packet (1 kHz); the other gyro
  samples repeat it.
- No physics: the attitude and the rates are what `sim_feed.py` sends. Motor
  and servo outputs are not simulated (servos: none, `servoInit()` finds no
  pins).
- Blackbox over TCP: no data loss in the tests (500 Hz and 1 kHz logging, about
  10 and 20 KiB/s; `loopIteration` continuous, no corrupt frames). The TX
  buffer is 8 KiB; `blackboxWrite()` drops bytes when it is full, which needs
  the TCP thread (1 ms period) to stall for several ms.
- Time stamps: the host and the SITL use the same CLOCK_MONOTONIC; the SITL
  clock is real time times simRate, so the timeline is accurate to the MSP
  and RC latency: a command shows in the log 2..50 ms after the host sends it.
