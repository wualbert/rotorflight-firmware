# RF-PARAM-1: exact parameter provenance in Rotorflight 4.6 blackbox logs (final merged specification)

Status: final design, not built. Date: 2026-10-06.

- **Firmware base.** Rotorflight 4.6.0, tag `release/4.6.0` = `118e912`, clone `scratchpad/src/fw-4.6.0`. Firmware paths are relative to `src/main/` unless a path says otherwise.
- **Viewer.** `/Users/albertwu/exp/rotorflight-blackbox`.
- **Verification.** I read every firmware `file:line` in this spec in the clone, and every viewer `file:line` in the repo.
- **Build numbers.** They come from:
  - `scratchpad/src/build_g47x_base.log` and `build_f7x2_base.log` (baseline);
  - `scratchpad/design/minimal-diff/build_STM32G47X_patched.log` (the built prototype).
- **Tables.** The value-table and PG-size numbers come from the decoded ELF tables in `design/provenance-safe/vt_*.json` and `design/minimal-diff/vtable_*.json`.

---

## 0. Decision and lineage

### 0.1 What a log carries

1. **A header snapshot (`H` lines).** It holds every byte of 30 tracked parameter groups (PGs), including all 6 PID profiles and all 6 rate profiles:
   - bytes that the CLI names are written as CLI `get` text;
   - servo, mixer-input, mixer-rule, rx-failsafe and feature data are written as element lines in CLI print order;
   - any remaining byte is written as raw hex;
   - for boot-cached PGs, the boot values are added where they differ.

   An end line carries a line count and a hash.
2. **Four new slow-frame (S-frame) fields: `pidProfile`, `rateProfile`, `armed`, `paramSeq`.** Every frame inherits them, so every frame has its profile indices, its arm state and the number of the last journal record that can apply to it.
3. **A change journal in event 101 (`CUSTOM_STRING`).**
   - The task that changes the configuration captures each change synchronously, byte-exact, and stamps it with `(blackboxIteration, pidUpdateCounter)`.
   - The record is stored in a RAM ring. The PID task writes it after a frame, under a byte budget and a free-space reserve.
   - Each record has a sequence number, the old and the new value, and a CRC.
   - Loader runs (`A`), runtime readbacks and fingerprints (`R`), markers (`M`), losses (`L`) and an end record (`Q`) use the same stream.
4. **No change to flight behaviour.** Parameters are applied when and how 4.6.0 applies them. An optional, separate set of behaviour fixes (PR-C, section 3.11) makes some deferred paths apply at once.

`blackbox_params = OFF | CHANGES | FULL` is a new CLI setting. OFF gives a log that is byte-identical to 4.6.0. The fork default is FULL, and the proposed upstream default is CHANGES. A serial logger below 1 Mbaud gets CHANGES when the setting is FULL (3.12), and the `param_mode` line tells it.

### 0.2 Base and grafts

| | minimal-upstream | exactness-first (PJ) | resource-compat (RF-PARAM-1) |
|---|---|---|---|
| exactness judge | 4 | 8 | 6 |
| safety and resources judge | 6 | 4.5 | 7.5 |
| compatibility and upstream judge | 7 | 3.5 | 6.5 |
| **total** | 17 | 16 | **20 (base)** |

The base is resource-compat (RF-PARAM-1). From it this spec keeps:
- the header snapshot of all 6+6 profiles;
- the S-frame carrier;
- the budgeted, reserve-checked drain;
- the `boot.*` lines;
- the three-valued runtime toggle;
- the bounded `LOG_END` hold;
- the rule that an unclassified effect is shown as unknown.

Grafted from exactness-first (PJ):
- capture in the task that makes the change, with a `(blackboxIteration, pidUpdateCounter)` stamp;
- a sequence number and a CRC-8 for each record;
- the S-frame sequence carrier (`pjSeq` here is `paramSeq`);
- stamped loader-apply records (`A`);
- runtime readbacks and fingerprints (`R`);
- T0 is the first RUNNING entry of the log;
- the runtime phase map;
- hooks at EEPROM load and save, CMS, stick trims and accelerometer calibration;
- the rule that a reader ignores unknown record types and fields.

Grafted from minimal-upstream:
- the phase rule `c <= b ? c : 0`;
- the `pg->copy` shadow at 0 B of RAM, with its built and size-measured prototype, host test, CLI formatter and header writer, which can be ported;
- the old value in each record;
- the toggle appended to `blackboxConfig_t` with no PG version bump;
- between-log edits, as `v` records;
- the behaviour fixes (PR-C);
- the PR split.

Grafted from the judges:
- a word-first diff with a changed-word bitmap;
- no newlib-nano `memcpy`/`memcmp` in hot paths;
- no `schedulerIgnoreTaskExecTime()` outside the realtime task;
- a pre-capture, so that a hook never claims a difference that it did not cause;
- events of at most 128 chars (the decoder limit is 252);
- a self-contained log, because rolling erase is on by default;
- escaping of `<>&`.

### 0.3 Fatal flaws fixed

Appendix A has the full ledger with `file:line` evidence. In short:

| Flaw (design) | Fix in this spec |
|---|---|
| "source `adj` means in force" is false (resource-compat) | No rule from the source. Running values come from stamped `A` records, `boot.*` values and a per-firmware semantics table in the viewer. The fingerprints check that table. |
| No `p > b` exception in the split rule. The filter task is ignored (resource-compat) | The phase rule in 2.5 and 3.5. The phase map is recorded at runtime, and the filter task ticks come from `activeFilterLoopDenom`. |
| The scan bound is counted in logged iterations (resource-compat) | Each PG has its own verified point (`bbpVerified[t]`), a real `(iteration, tick)` stamp. |
| After an overflow, a later hook steals the stale differences (resource-compat) | `lostMask` plus a pre-capture. Those bytes are always recorded with source `y` and an interval. |
| Header-window captures are stamped `0.1` (all designs) | Before T0 the stamp is `p`, and no frame is affected. |
| Apply bits attach to the next capture (resource-compat) | Each loader writes its own stamped `A` record after a capture of its region. |
| The MSP hook misses the out-with-arg branch (minimal) | The operation brackets the whole dispatch except the two read-only branches (`msp.c:4083-4086`). |
| A deferred compare gives unhooked changes a wrong exact stamp (minimal, PJ) | The pre-capture at the start of every operation. |
| No per-frame carrier, no CRC, no end record (minimal) | S-frame `paramSeq`, a CRC-8 for each event, and the `Q` record. |
| The PID task diffs 200-265 µs per drain. The `LOG_END` drain is unbounded. A 4.8 KB nano `memcpy` (minimal) | The capture runs in the task that makes the change, with a word diff. At most one event is written per logged iteration. The hold is at most 100 ms. No bulk copy at log start. |
| 38 KB of snapshot streamed after T0. A 128 B reserve. A compare on every MSP command. No runtime OFF (PJ) | The snapshot is written before T0. The reserve is 192 B. Read and skip-list commands do no work. The runtime OFF exists. |
| B, P and R at T0 are dropped by the state gate (PJ) | Nothing is written before RUNNING. The T0 hook runs after the state assignment (`blackbox.c:2152-2153`). |
| An adjustment refresh can name only one field (PJ) | The viewer table lists a set of runtime fields for each adjustment (`governor.c:1359-1377`). |
| Records up to 255 chars (minimal) | At most 128 chars. |
| Raw hex for the servo and mixer PGs (resource-compat) | Element formatters. Raw hex is kept only for the remainder. |

---

## 1. Problem and where it is specified

### 1.1 Requirement

For any time point (frame) of a blackbox log, the user needs the exact values of the flight parameters that produced it, from the log alone. A CLI dump is optional.

### 1.2 Why stock 4.6.0 cannot meet it

1. **The header is a partial, live sample.**
   - `blackboxWriteSysinfo` (`blackbox/blackbox.c:1587-1789`) writes about 90 `H` lines, one line per call (`:1591` reserves 64 B per call). It writes them after a 100 ms wait (`:2083`), while the craft is armed.
   - The PID and rate lines are those of the profiles active when the line is written (`:1595`, and for example `rollPID` at `:1649-1653`, `govPID` at `:1668-1672`).
   - No PID or rate profile index is in the header or in the I, P or S frames (`:334-341`).
   - 224 flight-relevant CLI settings are in no form in the log (brief; `understand.json`).
   - In the data, 58 % of the Fireball armed time is on a PID profile whose values are in no header, and there are 283 in-flight profile switches.
2. **One header covers several arms.**
   - After a disarm the log stays in GRACE for `blackbox_grace_period` s (`blackbox.c:1212-1250`; the default of 5 s is at `pg/blackbox.c:65`).
   - A re-arm continues the log with no header and no event (`:2173-2186`).
   - 36 of 164 logs hold 2-8 arms.
3. **MSP writes are applied while armed, with no record.**
   - All MSP goes through `mspFcProcessCommand` (`msp/msp.c:4074-4102`): serial at `fc/tasks.c:157-158`, MSP over telemetry at `telemetry/msp_shared.c:153`, and nested `MSP_MULTIPLE_MSP` at `msp.c:2344-2378`.
   - Examples: `MSP_SELECT_SETTING` (`:2486-2501`), `SET_PID_TUNING` (`:2560-2577`), `SET_RC_TUNING` (`:2628-2657`), and `SET_SERVO_CONFIG` in the out-with-arg branch (`:2224-2249`).
   - `MSP_EEPROM_WRITE` while armed is refused but sets the dirty flag (`:3133-3137`). The save then runs at disarm + 0.5 s (`fc/core.c:452-460`).
4. **Only adjustments leave a trace.**
   - `processRcAdjustments` calls `cfgSet` (`fc/rc_adjustments.c:356`) and logs `INFLIGHT_ADJUSTMENT` (`:361`).
   - Event 101 exists (`blackbox/blackbox_fielddefs.h:119`, helper at `blackbox.c:1872`), but nothing in the firmware calls it.
5. **The stored value is not always the running value.**
   - **Boot only:**
     - governor config (`flight/governor.c:1611-1672`, called only from `fc/init.c:799`);
     - `debug_mode` (`init.c:411`);
     - the loop denominators (`init.c:685-691`, `sensors/gyro_init.c:629-630`);
     - the servo rate, which is written back at boot (`flight/servos.c:190`).
   - **Deferred:**
     - `MSP_SET_RESET_CURR_PID` does not reload (`msp.c:2851-2853`);
     - `MSP_COPY_PROFILE` does not reload while armed, and its rate branch compares with the PID index (`msp.c:2503-2517`, `:2515`);
     - the setpoint-boost adjustments write only the profile (`flight/setpoint.c:96-129`), while the runtime gain is cached at `:225`;
     - the rc-rate, s-rate and expo adjustments (`fc/rc_rates.c:71-163`) change the live rate curve, but the cyclic ring limit stays cached (`setpoint.c:237-240`);
     - `MSP_SET_RC_CONFIG` does not call `initRcProcessing` (`msp.c:3545-3553`, `fc/rc.c:226-246`);
     - the disarm save runs `validateAndFixConfig` with no `activateConfig` (`config/config.c:759-769`).
   - **Runtime only:** `gov.govMode` falls to NONE when there is no RPM source (`governor.c:1625-1633`).
6. **Changes can split a frame.**
   - Both helicopters use `pid_process_denom = 2`. Tick 0 runs setpoint, PID and flush. Tick 1 runs mixer, motors, filter update and `blackboxUpdate` (`fc/core.c:899-913`).
   - `pidUpdateCounter` advances at the end of each tick (`core.c:1056`).
   - Only gyro, filter and PID run in the realtime block (`scheduler/scheduler.c:521-529`). The MSP, RX and CMS tasks run between ticks.
7. **Transport hazards.**
   - The flashfs write ring is 512 B and drops bytes silently when full (`io/flashfs.h:20-21`, `io/flashfs.c:576-590`).
   - W25N writes block on G4 (`drivers/flash_w25n.c:38-42`).
   - The decoders reject an event frame above 256 B (`js/flightlog_parser.js:16`, `:1788`), so a string can have at most 252 chars.
   - The viewer drops events that come before the first I-frame (`js/flightlog_index.js:157-160`).
   - Rolling erase is on by default (`pg/blackbox.c:64`; `USE_FLASHFS_LOOP` at `target/STM32_UNIFIED/target.h:91`; `io/flashfs.c:371-390`). So a log must not depend on another log.

### 1.3 The guarantee (FULL mode)

For every logged frame F and every tracked parameter byte, the log gives one of these:
- (a) the exact stored value that each subtask of F used, with F marked as mixed when a change fell between its ticks;
- (b) a flag that says the value is uncertain or unknown in a stated interval, and why.

The running value (the value that a computation used) comes from the stored values, the stamped `A` records, the `boot.*` values and the viewer's semantics table. The viewer flags it when the table does not classify the key. No value is carried over silently.

---

## 2. Log format additions

### 2.1 Versioning and modes

- **Version key.** `H Param log:1`. A log without it is a stock log.
  - Version 1 may later get new header keys, new record types and new fields.
  - A reader ignores the keys, types and `name=value` fields that it does not know.
  - A change of meaning of an existing item needs `Param log:2`.
- **Mode line.** `H param_mode:FULL` or `CHANGES`. OFF writes nothing.

| Mode | Header | S-frame | Journal | Exact? |
|---|---|---|---|---|
| OFF | 4.6.0 | 4.6.0 (5 fields) | none | no (stock) |
| CHANGES | meta lines (about 0.6 KB) | +4 fields | yes | profile indices and every change are exact. Values that never change in the log come only from the classic header. |
| FULL | meta lines and snapshot (about 15 KB) | +4 fields | yes | yes (1.3) |

### 2.2 Header section

The section is written by a custom case at the end of `blackboxWriteSysinfo`, after `fields_mask` (`blackbox.c:1779`). It is complete before CACHE_FLUSH and RUNNING, so before T0. Lines are in this order:

```
H Param log:1
H param_mode:FULL
H param_loop:<pid_denom>,<filter_denom>,<P interval>,<scan bytes per logged iteration>
H param_phase:pos=<t>,sp=<t>,pid=<t>,mix=<t>,mot=<t>,fupd=<t>,bb=<t>,flush=<t>
H param_pgs:<pgn>.<ver>/<size>,...
H param_fixes:<c1,c2,...>|-
H param_rt:gov_mode=<n>,features=<hex>,pid_denom=<n>,filter_denom=<n>,looptime=<us>,debug_mode=<n>,motors=<n>,servos=<n>
H param_pid_profile:<0-5>
H param_rate_profile:<0-5>
--- FULL only ---
H set.<name>:<value>
H set@p.<name>:<v0>|<v1>|<v2>|<v3>|<v4>|<v5>
H set@p<k>.<name>:<v>
H set@r.<name>:<v0>|...|<v5>
H set@r<k>.<name>:<v>
H el.servo.<i>[-<j>]:<mid>,<min>,<max>,<rneg>,<rpos>,<rate>,<speed>,<flags>
H el.mixin.<i>[-<j>]:<rate>,<min>,<max>
H el.mixrule.<i>[-<j>]:<oper>,<input>,<output>,<offset>,<weight>
H el.rxfail.<i>[-<j>]:<mode>,<step>
H el.feature:<hex enabledFeatures>
H pg.<pgn>+<off>:<hex>
H boot.<key>:<value>
--- all modes ---
H param_end:<lines>,<fnv32 hex>
```

Line meanings:

- **`param_loop`.** The PID and filter denominators that are in use (`activePidLoopDenom` and `activeFilterLoopDenom`), the blackbox P interval, and the scan step of 3.7.
- **`param_phase`.** The tick of each PID subtask, recorded at runtime (3.9). The gyro-filter task runs in tick k when `k % filter_denom == 0` (`core.c:856-859`).
- **`param_pgs`.** The tracked PGs in snapshot order: `pgN` and `pgVersion` (`pg/pg.h:60-61`) and the RAM size.
- **`param_fixes`.** The PR-C behaviour fixes that are compiled in (3.11).
- **`param_rt`.** Runtime values that can differ from the configuration: the governor mode after a demotion, the runtime feature mask, the active denominators, the looptime, the debug mode, and the motor and servo counts.
- **`param_pid_profile` and `param_rate_profile`.** The `currentPidProfile` and `currentControlRateProfile` index when the line is written (0-based, as the CLI writes `profile N`).
- **`set.<name>`.** Each MASTER_VALUE and HARDWARE_VALUE entry of a tracked PG, in `valueTable` order.
- **`set@p.<name>`.** Each PROFILE_VALUE entry for PID profiles 0-5. An empty field means the same text as the previous profile.
- **`set@p<k>.<name>`.** Used for each profile instead of the `set@p.` line when that line would be longer than 192 chars.
- **`set@r.<name>`.** The same for PROFILE_RATE_VALUE entries and rate profiles 0-5, with the same `set@r<k>.` fallback.
- **`el.<kind>.<i>[-<j>]`.** One line per element. `i-j` is a run of elements with the same text.
- **`pg.<pgn>+<off>`.** Tracked bytes that no name and no element covers, at most 64 B per line. `off` is the byte offset in the whole PG, all elements included.
- **`boot.<key>`.** For boot-cached PGs only: the value at the end of `init()` where it differs from the value now. `key` is `set.<name>`, `el.<...>` or `pg.<...>`.
- **`param_end`.** The line count and hash of the section (see the rules below).

**Value text.** It is the same text that CLI `get` prints (`cli/cli.c` `printValuePointer`):
- integers are decimal;
- a lookup prints its name, or `?<n>` when the value is out of range;
- a bitset prints `ON` or `OFF`;
- an array prints values separated by commas;
- an empty string prints `-`;
- in strings, a space, `% | , < > & * = :` and every non-printable byte are written as `%XX`.

**Line rules.**
- A line has at most 192 chars. The decoder keeps lines up to 1024 chars.
- `param_end`:
  - `lines` is the number of lines from `Param log` to the line before `param_end`;
  - the hash is FNV-1 as in `fnv_update` (`common/crc.c:128-139`, basis `FNV_OFFSET_BASIS` at `common/crc.h:50`), over the bytes `H <key>:<value>\n` of those lines.
- **Snapshot consistency.**
  - Each line is printed from live RAM while hooks are active. A change that comes before a line is printed shows in the line. Every change during the header is also a journal record stamped `p` (2.5).
  - Thus the state at T0 is the header with the `p` records applied in sequence order.
  - No frame exists before T0 (`blackbox.c:2135-2154`).

**Tracked set (30 PGs, 2,679 B on G474).**
- Profiles and system: PID_PROFILE (912 = 6 × 152), CONTROL_RATE_PROFILES (300 = 6 × 50), SYSTEM_CONFIG, PID_CONFIG.
- Sensors and filters: GYRO_CONFIG, GYRO_DEVICE_CONFIG, ACCELEROMETER_CONFIG, BOARD_ALIGNMENT, IMU_CONFIG, DYN_NOTCH_CONFIG, RPM_FILTER_CONFIG.
- Governor, motor and RPM: GOVERNOR_CONFIG, MOTOR_CONFIG, ESC_SENSOR_CONFIG, FREQ_SENSOR_CONFIG.
- Mixer and servos: GENERIC_MIXER_CONFIG, GENERIC_MIXER_INPUTS, GENERIC_MIXER_RULES, SERVO_PARAMS.
- RX, failsafe and arming: RC_CONTROLS_CONFIG, RX_CONFIG, RX_FAILSAFE_CHANNEL_CONFIG, FAILSAFE_CONFIG, ARMING_CONFIG.
- Power: BATTERY_CONFIG, VOLTAGE_SENSOR_ADC_CONFIG, CURRENT_SENSOR_ADC_CONFIG.
- Other: FEATURE_CONFIG, BLACKBOX_CONFIG, POSITION.

Excluded PGs:
- STATS_CONFIG changes at every disarm (`fc/stats.c:58-73`).
- ADJUSTMENT_RANGE_CONFIG and MODE_ACTIVATION_PROFILE: their effects are already in the log as `INFLIGHT_ADJUSTMENT` events and as the S-frame `flightModeFlags`.
- SERVO_CONFIG holds only pin tags.
- LED, OSD, VTX, telemetry, serial, pin, timer, GPS, barometer and compass PGs.

The snapshot holds 242 `set.` entries and 134 profile entries (102 PID and 32 rate).

**Boot-cached PGs (the boot copy, 860 B on G474).** GOVERNOR_CONFIG, MOTOR_CONFIG, GYRO_CONFIG, GYRO_DEVICE_CONFIG, DYN_NOTCH_CONFIG, RPM_FILTER_CONFIG, PID_CONFIG, SYSTEM_CONFIG (without the two profile-index bytes), ESC_SENSOR_CONFIG, FREQ_SENSOR_CONFIG, ACCELEROMETER_CONFIG, BOARD_ALIGNMENT, RX_CONFIG, GENERIC_MIXER_CONFIG, SERVO_PARAMS.

**Size (FULL).** About 14-16 KB: 242 × about 32 B, plus 134 profile lines with empty-field compression, plus about 2 KB of elements. Section 3.12 gives the cost in time.

### 2.3 S-frame fields

Append to `blackboxSlowFields[]` (`blackbox.c:334-341`) and to `blackboxSlowState_t` (`:442-448`, packed, so the `memcmp` at `:1129-1136` forces an S-frame on any change):

```
{"pidProfile",  -1, UNSIGNED, PREDICT(0), ENCODING(UNSIGNED_VB)}   // currentPidProfile - pidProfiles(0), 0-5
{"rateProfile", -1, UNSIGNED, PREDICT(0), ENCODING(UNSIGNED_VB)}   // currentControlRateProfile - controlRateProfiles(0), 0-5
{"armed",       -1, UNSIGNED, PREDICT(0), ENCODING(UNSIGNED_VB)}   // ARMING_FLAG(ARMED)
{"paramSeq",    -1, UNSIGNED, PREDICT(0), ENCODING(UNSIGNED_VB)}   // seq of the last journal record created (0 = none)
```

- **Pointers, not indices.** The fields read the profile pointers, not `systemConfig()` indices, because `pidController(currentPidProfile, ...)` (`core.c:784`) and the loaders use the pointer.
- **When an S-frame is written.**
  - An S-frame is written before the main frame whenever a field changes (`blackbox.c:1118-1140`), at RUNNING entry (`:703-705`), and every 5 s.
  - So `paramSeq` at logged frame k is the number of the last record created before the `blackboxUpdate` of frame k.
- **OFF mode.** The field count sent by `sendFieldDefinition` (`:2129`) and the bytes written by `writeSlowFrame` (`:1085-1100`) are those of 4.6.0.

### 2.4 Journal events (event 101)

```
event  = "P" TYPE SEQ " " AT *(" " FIELD) "*" CRC        ; at most 128 chars
TYPE   = "C" | "A" | "R" | "M" | "L" | "Q" | "+"
SEQ    = 1*8 lowercase-hex
AT     = "p" | POINT | POINT "~" POINT
POINT  = ITER "." TICK
CRC    = 2 uppercase-hex
```

- **`SEQ`.** The record number in this log, starting at 1. A `+` event repeats the SEQ of its record.
- **`AT`.**
  - `p` means before T0.
  - `POINT "~" POINT` is an interval: the change came after the first point and before the second.
- **`POINT`.** `ITER` is the decimal `blackboxIteration`, which equals the frame `loopIteration` (`blackbox.c:724`). `TICK` is decimal, see 2.5.
- **`CRC`.** CRC-8, polynomial 0xD5 (`crc8_update`, `common/crc.h:26`), initial value 0, over all chars before `*`.

| Type | Fields | Meaning |
|---|---|---|
| `C` change | `s=<SRC>[.<arg>]` (`y`: `pgs=<pgn>` `[part=1]`) `n=<items>` then items `KEY=NEW<OLD` | Stored bytes changed. A record whose items do not fit in one event continues in `+` events with the same SEQ and AT. `n` counts the items of all its events. |
| `A` apply | `<loader>[/<slot>]` `[fp=<8 hex>]` | A loader ran at this point and read the stored values of its region. All `C` records with a lower SEQ were already applied. `fp` is the fingerprint of the runtime parameters of its module after the load. |
| `R` runtime | `gov_mode=<n>` `features=<hex>` `fp.pid=` `fp.gov=` `fp.sp=` (any subset) | Polled runtime values. Written at T0 (stamp `p`), then only on a change, with an interval stamp. |
| `M` marker | `eesave us=<n>`, `eeload`, `escparam n=<len> fnv=<hex>`, `shadow-reset` | Events that explain stalls or limits of the journal. |
| `L` lost | `pgs=<pgn>,...` | The RAM ring overflowed. The bytes of the listed PGs that were not recorded come back later in `y` records (2.6). |
| `Q` end | `unsent=<n>` `lostrec=<n>` | Written just before `LOG_END`. |

**Sources (`SRC`).**
- `m` is MSP, with the command as `arg`.
- `a` is an adjustment, with the function as `arg`.
- `k` is a CMS key.
- `p` is a profile change outside another operation, for example by the sticks.
- `e` is an EEPROM load.
- `w` is an EEPROM save.
- `t` is a stick trim.
- `c` is an accelerometer calibration.
- `l` is a loader outside an operation.
- `v` is a change between logs: since the end of the previous log of this boot, or since boot.
- `u` is unattributed: no hook saw the write.
- `y` is a resync after a ring overflow. A `y` record gives its PG as `pgs=<pgn>`, also when it has no items. A `y` record without items tells that the bytes of the PG are back at their recorded values. A `y` record that does not complete its PG (its items did not fit, or the PID task resyncs the PG in parts, 3.7) has `part=1`. A `y` record with `part=1` always has items.
- `u`, `v` and `y` records always carry an interval or `p`. A capture in the PID task (3.7) can give one PG in more than one record of the same source, each with its own end point.

**Keys.**
- `<name>` is a master or hardware value.
- `p<k>.<name>` is PID profile k, and `r<k>.<name>` is rate profile k (0-based).
- `<key>[<i>]` is one array element. Records give only the changed elements.
- `el.<kind>.<i>` is a whole element.
- `pg.<pgn>+<off>` is at most 16 raw bytes, as hex.
- `pid_profile` and `rate_profile` are the profile indices.

Values use the header text rules.

**Examples** (Fireball, denominator 2, so `bb = 1`; the CRC values are placeholders):

```
PC1a 48211.1 s=m.202 n=1 p0.yaw_p_gain=72<70*xx       Lua yaw P save while armed, between tick 0 and tick 1: frame 48211 is mixed
PA1b 48211.1 pid/0 fp=9c3a11f0*xx                      pidLoadProfile ran in the same command: the new gain is in force
PC1c 60100.0 s=a.2 n=1 pid_profile=1<0*xx              in-flight switch by the adjustment switch
PA1d 60100.0 pid/1 fp=1e0277a3*xx
PC1e 70100.1 s=m.211 n=1 gov_mode=ELECTRIC<DIRECT*xx   MSP_SET_GOVERNOR_CONFIG while armed: boot-cached, so pending until reboot
PM1f 82000.0 eesave us=71230*xx                        the disarm save (the 71 ms stall)
PR20 82001.0~82021.0 fp.gov=55aa0912*xx
PC21 83000.0~83022.0 s=u n=1 r0.cyclic_ring=150<140*xx a write that no hook saw (zero in a correct build)
```

### 2.5 Points and frames (normative)

Let `t(S)` be the tick of subtask S from `param_phase`. The gyro-filter task runs in tick k when `k % filter_denom == 0`. Let `b = t(bb)`.

- **Stamp `p`.** The change came before T0, which is the first RUNNING entry of the log. Every frame of the log used the new value.
- **Stamp `N.c`.** The change came after ticks `0..c-1` of the PID cycle that frame N records.
  - Frames with `loopIteration < N` used the old value. Frames `> N` used the new value.
  - In frame N, subtask S used the new value when `t(S) >= c`. With `c = 0`, frame N is new in all fields. With `0 < c <= b`, frame N is **mixed**.
  - The firmware never writes `c > b` (3.5).
  - These two rules are for a subtask with `t(S) <= b` only. A **late subtask** (`t(S) > b`: the filter update `fupd` with `pid_process_denom` 3, and `flush`) runs after `blackboxUpdate` in its cycle, so its run in cycle N acts on frame N+1. For a late subtask, frames up to N-1 are old and frames from N+1 are new. In frame N it is old when `c >= 1`, and **uncertain** when `c = 0`: the firmware also writes `N.0` for a change after tick b of cycle N-1 (3.5), and the late run of cycle N-1 can be before or after that change.
- **Stamp `N0.c0~N1.c1`.** The change came after point 0 and before point 1. Subtasks that ran before point 0 used the old value, and subtasks that ran after point 1 used the new value. Everything between is **uncertain** for the keys of the record.
- **Consumers outside the realtime block.** Examples are the RX task (`fc/rc.c:222` reads `rc_center` live, `fc/tasks.c:211`) and the ACC task (`fc/tasks.c:164` passes the trims by pointer). These consumers see a change at their next run after the point. The viewer marks frames up to one period of that task after the point as transition frames for those keys (4.3).

### 2.6 Fold rules for a reader (normative)

1. **Decode.**
   - Collect the event-101 strings that match `^P[CARMLQ+][0-9a-f]{1,8} `.
   - Check the CRC. A string with a bad CRC counts as lost.
   - Join the `+` events to their record and check `n`.
2. **Find the losses.** A loss is any of these:
   - a missing SEQ between two received records;
   - a **trailing gap**: any frame whose S-frame `paramSeq` is above the highest SEQ received;
   - a `Q` that reports `unsent > 0` or `lostrec > 0`.

   The time of a missing record lies between the points of its neighbours. For a trailing gap, it lies between the last frame that shows the previous `paramSeq` and the first frame that shows the missing one.
3. **Base state.**
   - The FULL snapshot, with the `p` records applied in SEQ order, gives the T0 state.
   - If `param_end` is missing or wrong, the snapshot is **incomplete**. A key that has no complete line is then unknown until a record sets it. The classic header line can be shown, labelled "header, not verified".
   - In CHANGES mode only the classic header and the records exist, and the same labels apply.
4. **Stored-value timeline of key K.**
   - Apply the `C` items in SEQ order.
   - An exact point sets K from that point.
   - An interval makes K uncertain inside the interval and exact after its end.
   - A `C.old` that differs from the believed value is a `chain-mismatch` flag. K is unknown back to the last record of K or to T0. Pre-T0 records are exempt, because a header line can come after a change.
5. **Losses.**
   - After a mid-log or trailing loss, every key is unknown from the earliest possible point of the lost record. Each key becomes exact again at its next `C` item, from that item's point.
   - After an `L` record, the keys of the listed PGs are unknown from the `L` point until their `y` record without `part=1`. The `y` items carry the value, and the `y` interval bounds the time. A `y` record with `part=1` sets only the keys of its items. The `y` record without `part=1` (it can have no items) ends the loss: the keys of the PG that no `y` record of the loss set have their value from before the `L`. The PG of a `y` record is its `pgs` field, not only the PGs of its items. An item of one array element (`<key>[<i>]`) of the loss sets that element: when the loss ends, the other elements of the key have their value from before the `L`.
6. **Mixed and uncertain frames.** Use 2.5.
7. **Running values.**
   - The viewer folds the `A` records with `param_semantics` (4.3). A key that the table does not classify is shown with its stored value and the flag `effect-unknown`.
   - The fingerprints check the table (4.3, step 6).
8. **Profile index of a frame.**
   - Take the S-frame `pidProfile` and `rateProfile`.
   - Cross-check them with the `pid_profile` and `rate_profile` items. A disagreement is a `chain-mismatch` flag.

### 2.7 Compatibility

| Consumer | New header lines | New S fields | Event 101 | Result |
|---|---|---|---|---|
| This viewer today | `unknownHeaders` (about 700 rows) | decoded and merged into frames | decoded (`flightlog_parser.js:1580-1583`) and drawn as labels (`grapher.js:578-579`) | opens and plots. Section 4 adds the parameter use. |
| RF blackbox explorer 2.x | the same | the same | the same | opens and plots |
| A decoder without event 101 | ignored | generic | an unknown event costs frames until the next I-frame | use CHANGES or OFF |
| This viewer on stock logs | none | none | none | falls back to `param_epochs.cjs` (5.3) |

Events are written only after a frame, so the bug that drops events before the first I-frame does not affect them. That bug is still fixed (4.1), because the viewer loses SYNC_BEEP to it.

---

## 3. Firmware changes

### 3.1 Files

**New files.**

| File | Contents |
|---|---|
| `blackbox/blackbox_params.c` | state, ring, capture, operations, drain, scan, runtime poll |
| `blackbox/blackbox_params_format.c` | CLI-identical value formatter, element formatters, header line generator |
| `blackbox/blackbox_params_tables.c` | tracked set, boot set, element descriptors, loader regions, skip list |
| `blackbox/blackbox_params.h` | the API, with static inline no-ops when `!USE_BLACKBOX` |
| `src/test/unit/blackbox_params_unittest.cc` | unit test |

- The formatter and the header line generator are ported from the host-tested prototype `design/minimal-diff/fw/src/main/blackbox/blackbox_settings.c`, functions `putValue`, `putKey`, `putString` and `nextHeaderLine`.
- In `make/source.mk`, add the three `.c` files after `blackbox/blackbox_io.c` (`:144-146`). Put `_format.c` and `_tables.c` in SIZE_OPTIMISED_SRC.

**Edits:** `blackbox.c`, `blackbox_io.c/.h`, `fc/core.c/.h`, `msp/msp.c`, `fc/rc_adjustments.c`, `config/config.c`, `fc/rc_rates.c`, `cms/cms.c`, `sensors/acceleration_init.c`, `fc/init.c`, `cli/cli.c`, `pg/blackbox.c/.h`, `cli/settings.c/.h`. Each edit is 1-4 lines.

**Loader markers** (one line each): `flight/pid.c`, `flight/governor.c`, `flight/rescue.c`, `flight/setpoint.c`, `sensors/gyro_init.c`, `flight/rpm_filter.c`, `flight/mixer.c`, `fc/rc_controls.c`, `config/config.c`, `config/feature.c`.

**Read-only accessors** for the fingerprints: `pid.c`, `governor.c`, `setpoint.c`, `feature.c`.

No line changes a flight computation. PR-C (3.11) is the separate, optional set of behaviour fixes.

### 3.2 Shadow and boot copy

- **Shadow = `pgRegistry_t.copy`** (`pg/pg.h:45-56`), which costs 0 B.
  - Only the CLI uses `copy`: `backupPgConfig` and `restorePgConfig` (`cli/cli.c:735`, `:741`), `backupConfigs` (`:745-756`) and `cliGetValuePointer` (`:834-842`). A grep finds no other user.
  - The CLI stops `blackboxUpdate` (`fc/core.c:830`), and `cliExit` always reboots (`cli/cli.c:4006-4020`).
  - Custom defaults (`cli.c:4477-4486`, reached from `MSP_RESET_CONF`, which is refused while armed, `msp.c:2433`) write to live RAM, because `configIsInCopy` is false outside the CLI (`cli.c:745-781`).
  - **Guard.** `backupConfigs()` calls `blackboxParamsShadowLost()`, which sets `shadowValid = false`. The next log then resyncs the shadow and writes `M shadow-reset` instead of `v` records.
  - **Records that a log did not get.** A capture syncs the shadow when it makes a record. When a log ends with records in the ring (an arm blip in a header state, the device full, an erase, or `unsent > 0` at `LOG_END`), `blackboxParamsStop()` also sets `shadowValid = false`. The next log then writes `M shadow-reset`, so the loss is explicit and no change is carried over silently.
  - A unit test asserts that `->copy` is used only in `cli.c` and `blackbox_params.c`.
  - Build option `BLACKBOX_PARAMS_OWN_SHADOW` uses its own 2,683 B buffer instead. One inline accessor, `SHADOW(t)`, is the only difference.
- **Boot copy** (860 B on G474). `blackboxParamsBoot()` runs just before `systemState |= SYSTEM_STATE_READY` (`fc/init.c:1018`). By then `servoInit` (`init.c:700`) and `governorInit` (`init.c:799`) have run. It does these things:
  - copies the boot-cached PGs into `bbpBoot`;
  - copies every tracked PG into the shadow with a word loop;
  - sets `shadowValid = true`.

### 3.3 State and RAM (G474)

```c
#define BBP_TRACKED        30
#define BBP_RING_SIZE      1024   // binary records
#define BBP_EVENT_MAX      128    // chars in one event 101 (decoder limit 252)
#define BBP_FRAME_RESERVE  192    // device bytes left free for frames after an event (about 4 frames of 41-48 B)
#define BBP_SCAN_BYTES     128    // compared per logged iteration
#define BBP_HOLD_MS        100    // longest LOG_END delay
#define BBP_PRE            0xFFFFFFFFu

typedef struct { uint32_t n; uint8_t c; } bbpPoint_t;     // n = BBP_PRE before T0

typedef struct {                  // built by blackboxParamsInit() with pgFind() and a pass over valueTable[]
    const pgRegistry_t *reg;
    uint16_t vtFirst, vtCount;    // contiguous valueTable run of this PG (asserted; if not contiguous, the walk covers the whole table)
    uint16_t elem;                // element descriptor or 0xFFFF
    uint16_t bootOff;             // offset in bbpBoot or 0xFFFF
} bbpPg_t;

static bbpPg_t    bbpPg[BBP_TRACKED];          // 360 B
static bbpPoint_t bbpVerified[BBP_TRACKED];    // 240 B with padding: last point where shadow == live was proved for the whole PG
static uint8_t    bbpBoot[BBP_BOOT_BYTES];     // 860 B
static uint8_t    bbpRing[BBP_RING_SIZE];      // 1,024 B
static char       bbpLine[196];                // one header line or one event
static struct {
    bool     active, shadowValid, t0, lineReady, holding, lostWritten;
    uint8_t  opDepth; char opSrc; uint16_t opArg;
    uint32_t seq;                              // records created in this log = S-frame paramSeq
    uint32_t prevMask, lostMask;               // PGs not yet compared with the previous log; PGs with unrecorded differences
    bbpPoint_t lostAt;
    uint16_t ringHead, ringTail, drainItem;
    uint8_t  scanPg; uint16_t scanOff; bbpPoint_t scanPassStart;
    uint8_t  rIdx, rTick; uint32_t rLast[5]; bbpPoint_t rPolled[5];
    uint8_t  hdrStage; uint16_t hdrIdx; uint8_t hdrSlot; uint32_t hdrHash; uint16_t hdrLines;
    uint16_t unsent, lostRec; timeMs_t holdStart;
} bbp;                                         // about 110 B
```

The total is about 2.8 KB.
- **G474.** 10,336 B are free (124 KB region, 116,640 B used), so about 7.5 KB stay free.
- **F7X2.** DTCM has 24,936 B free, and SRAM1 has 89,612 B free.
- **Stack.** The capture needs at most about 200 B of stack (an 8-word changed-word bitmap plus locals) in the MSP, RX or CMS task. Measure it in the bench test (6.4).

### 3.4 The capture

```c
// Compare the PGs in 'set' (one PID or rate slot only when slot >= 0) with the shadow. Each difference becomes an
// item with the new and the old bytes. The shadow is synced item by item. 'attributed' == false: each PG with a
// difference gets its own record with an interval that starts at bbpVerified[t].
static void bbpCapture(bool attributed, char src, uint16_t arg, bbpPoint_t at, uint32_t set, int8_t slot)
{
    rec_t *r = NULL;
    for (int t = 0; t < BBP_TRACKED; t++) {
        if (!(set & BIT(t))) continue;
        uint32_t words[8];                                   // changed-word bitmap; 912 B = 228 words at most
        if (!bbpWordDiff(LIVE(t), SHADOW(t), SIZE(t), slotRange(t, slot), words)) {   // 4-byte loads, unrolled
            if (slot < 0) bbpVerified[t] = at;
            continue;
        }
        char s = src; bbpPoint_t from = at;
        if (!attributed || (bbp.lostMask | bbp.prevMask) & BIT(t)) {      // a difference that this operation did not cause
            s = (bbp.lostMask & BIT(t)) ? 'y' : (bbp.prevMask & BIT(t)) ? 'v' : 'u';
            from = (at.n == BBP_PRE) ? at : bbpVerified[t];
            recCommit(r); r = NULL;                                       // its own record
        }
        if (!r && !(r = recBegin('C', s, arg, from, at))) goto lost;      // the seq is used even when nothing fits
        if (!walkNamed(t, words, r) || !walkElements(t, words, r) || !walkRaw(t, r)) goto lost;
        bbp.prevMask &= ~BIT(t); bbp.lostMask &= ~BIT(t);
        if (slot < 0) bbpVerified[t] = at;
        if (s != src) { recCommit(r); r = NULL; }
    }
    recCommit(r);                                            // an empty 'C' record is dropped
    return;
lost:
    recCommitLost(r);                                        // items that fit stay valid
    bbp.lostMask |= remainingDiffering(set);                 // their shadow stays unsynced
    bbp.lostAt = at;                                         // the PID task writes 'L' from lostMask and lostAt
}

// walkNamed: for each valueTable entry v of PG t (vtFirst..vtFirst+vtCount) and each instance k (PROFILE/RATE: 0-5,
//   stride = element size; cli.c:819-831 uses the same layout):
//   - test the bitmap words that the bytes of (v, k) cover: a few cycles, no memcmp
//   - BITSET: compare and copy only its bit; ARRAY: one item per changed element; STRING: compare up to maxlength
//   - otherwise compare the 1-4 bytes; item(key, new, old) into the ring; copy into the shadow
// walkElements: servo / mixin / mixrule / rxfail / feature, and the pidProfileIndex / activeRateProfile bytes of
//   SYSTEM_CONFIG (keys pid_profile / rate_profile)
// walkRaw: compare the PG again; each run that still differs (at most 16 B) becomes a pg.<pgn>+<off> item; sync
```

- **Ring format.**
  - Record header (20 B): `{u8 type; char src; u16 arg; u32 seq; u32 n0; u8 c0; u32 n1; u8 c1; u16 len}`.
  - Item: `{u16 code; u8 slot; u8 len; u8 new[len]; u8 old[len]}`.
  - `code` is a valueTable index (`< 0x8000`), an element (`0x8000 | kind << 8 | i`), raw bytes (`0xC000 | t`, followed by a `u16` offset), or `pid_profile` or `rate_profile`.
- **Cost.** The cost depends on the changed slots, not on entries × profiles. A changed PID slot costs at most 102 bitmap tests (about 1k cycles) plus about 50 cycles per item. A changed PID_PROFILE no longer costs 654 nano-`memcmp` calls. Copies and compares use word loops, never newlib-nano `memcpy` or `memcmp`, which work one byte per loop.
- **No scheduler call.** No capture calls `schedulerIgnoreTaskExecTime()`. Because of this, the scheduler sees the real task time (`scheduler.c:296-308`, `:650-668`).

### 3.5 Points

```c
static bbpPoint_t bbpNow(void)          // in a task outside the realtime block (MSP, RX, CMS, dispatch)
{
    if (!blackboxIsLogRunning()) return (bbpPoint_t){ BBP_PRE, 0 };     // state before RUNNING: before T0
    const uint8_t c = getPidUpdateCounter();    // ticks 0..c-1 of this cycle have run (core.c:1056)
    const uint8_t b = coreSubtaskTick(CORE_ST_BLACKBOX);
    return (bbpPoint_t){ blackboxGetIteration(), (c <= b) ? c : 0 };
    // c > b: blackboxUpdate of this cycle already advanced the iteration (blackbox.c:2171, 2185). The subtasks of frame
    // (iteration) with t(S) <= b run after the change: phase 0. The late subtasks (t(S) > b: flush, and the filter
    // update when the denominator is 3) run in this cycle and act on frame (iteration): uncertain there (2.5).
}
static bbpPoint_t bbpNowInPidTask(void) // after the frame of iteration N, before the advance
{
    return (bbpPoint_t){ blackboxGetIteration() + 1, 0 };
}
```

- `blackboxIsLogRunning()` is true in RUNNING, GRACE_PERIOD and PAUSED. These are the same states in which events are allowed (`blackbox.c:1797-1802`).
- `blackboxIteration` is 0 from `blackboxStart` (`:1161-1164`) until the first frame, and it advances only in those states (`:2171`, `:2185`). So the `p` stamp is exact, and frame 0 is never falsely mixed.

### 3.6 Operations and loader markers

An **operation** is one mutating entry point. It does a **pre-capture**, then its work, then a **post-capture**. Operations nest: only the outermost one captures and gives the source.

```c
void bbpOpBegin(char src, uint16_t arg)
{
    if (!bbp.active || bbp.opDepth++ > 0) return;
    bbp.opSrc = src; bbp.opArg = arg;
    bbpCapture(false, 'u', 0, bbpNow(), BBP_ALL, -1);   // differences that exist before the operation: u / v / y with an interval
}
void bbpOpEnd(void)
{
    if (!bbp.active || bbp.opDepth == 0 || --bbp.opDepth > 0) return;
    bbpCapture(true, bbp.opSrc, bbp.opArg, bbpNow(), BBP_ALL, -1);    // the operation's own writes, exact point
}
void bbpApplied(bbpLoader_e l, uint8_t slot)    // at the effective end of each loader
{
    if (!bbp.active) return;
    const bool outside = (bbp.opDepth == 0);
    if (outside) bbpOpBegin('l', l);
    bbpCapture(true, bbp.opSrc, bbp.opArg, bbpNow(), loaderRegion[l], regionSlot(l, slot));  // writes that the loader read: C before A
    recApply(l, slot, bbpNow(), bbpFingerprint(l));
    if (outside) bbpOpEnd();
}
```

**Loader regions.** They are const and generous, and the static audit in 6.2 checks them:

| Loader | Region |
|---|---|
| `pid` | the PID slot, PID_CONFIG, SYSTEM_CONFIG, GOVERNOR_CONFIG, MOTOR_CONFIG |
| `gov` | the PID slot, GOVERNOR_CONFIG, MOTOR_CONFIG |
| `rsc` | the PID slot |
| `sp` | the rate slot, SYSTEM_CONFIG |
| `gyrof` | GYRO_CONFIG, DYN_NOTCH_CONFIG, PID_CONFIG |
| `rpmf` | RPM_FILTER_CONFIG, MOTOR_CONFIG, FREQ_SENSOR_CONFIG |
| `mix` | the three mixer PGs, SERVO_PARAMS |
| `rcctl` | RC_CONTROLS_CONFIG, RX_CONFIG |
| `act` | all tracked PGs |
| `feat` | FEATURE_CONFIG |

- **Writes inside a loader.** A loader can write its own region, for example `governorInitProfile` calls `validateAndFixGovernorProfile`. Those writes are captured as `C` before its `A`, with the operation's source.
- **A region that misses a PG.** If a region misses a PG that the loader reads, a write to that PG earlier in the same operation comes after the `A` in SEQ order. The viewer cannot then order it. Its semantics table names the PGs that each loader reads. When such a `C` with the same point follows the `A`, the viewer flags `order-unknown` for that key. It does not guess.

### 3.7 PID-task work

```c
void blackboxParamsAfterFrame(void)     // blackboxLogIteration() after the frame (logged iterations); PAUSED: every iteration after the first frame
{
    if (!bbp.active || !bbp.t0) return;
    const bbpPoint_t now = bbpNowInPidTask();
    if (bbp.lostMask && !bbp.lostWritten) recLost(bbp.lostAt);              // 'L' (the ring keeps 24 B free for it)
    drainOneEvent();
    if (step.t >= 0 || bbp.lostMask) stepGroups(lostMask, now, 256);        // one step of a capture: 'y' (or 'u') records, interval from bbpVerified[t]
    else scanStep(now);                                                     // BBP_SCAN_BYTES; a difference starts a capture in steps: 'u' records
    if ((++bbp.rTick & 3) == 0) pollOneRuntime(now);                        // gov_mode, features, fp.pid, fp.gov, fp.sp
}

static void drainOneEvent(void)         // at most one event per logged iteration
{
    if (!bbp.lineReady) {
        if (ringEmpty()) return;
        formatNextEvent(bbpLine, BBP_EVENT_MAX);    // C/A/R/M/L/Q head or '+' continuation; whole items only
        bbp.lineReady = true;
    }
    if (blackboxDeviceFreeSpace() < (int32_t)strlen(bbpLine) + 3 + BBP_FRAME_RESERVE) return;   // try after the next frame
    blackboxLogCustomString(bbpLine);   // 'E', 101, length, chars (blackbox.c:1843-1848)
    bbp.lineReady = false;              // the ring advances when the last event of a record is written
}

static void scanStep(bbpPoint_t now)
{
    // Compare the next BBP_SCAN_BYTES of PG scanPg from scanOff. A PG larger than the step is compared over several
    // logged iterations: bbpVerified[t] becomes the point at which the pass of that PG STARTED (scanPassStart),
    // never the point at which it ended. A difference: bbpCapture(false, 'u', 0, now, BIT(t), -1).
}
```

- **Captures in steps.** The PID task never captures a whole PG in one call. The resync (`y`), a difference that the scan finds (`u`) and the compare with the previous log in the header states (`v`, 3.8) are captures in steps with a limit of work (`BBP_STEP_UNITS`, 128 units of about 10 cycles: a compare of 32 B is 11, a valueTable entry 8, an item 12, a formatted element 100). One step compares the next blocks of the PG into the changed-word bitmap, or walks the next entries, elements or raw bytes and makes their items. Each step with items is a record of its own, with the interval from `bbpVerified[t]` to the point of that step. A `y` record that does not complete the PG has `part=1` (2.4). When the walk is done, `bbpVerified[t]` becomes the point of the first compare step, as for the scan. Any other capture of the PG (an operation, a loader) stops the steps: its own record has the differences that are left.
- `blackboxDeviceFreeSpace()` is moved out of `blackboxReplenishHeaderBudget` (`blackbox_io.c:665-687`) with no change of behaviour.
- **Bytes per iteration.** At most one event, up to 131 B, is written per logged iteration, and only when 192 B stay free for frames afterwards. So the journal can never cause a flashfs drop. On G4 it adds at most 131 B to a flush.
- **Fingerprints.** They are FNV-1 over 32-bit words of the parameter fields only, never loop state:
  - `fp.pid`: what `pidLoadProfile` writes (`pid.c:583-717`).
  - `fp.gov`: what `governorInitProfile` writes (`governor.c:1543-1601`) plus `gov.idleThrottle` and `gov.autoThrottle`, but not `requestedHeadSpeed`.
  - `fp.sp`: what `setpointInitProfile` writes (`setpoint.c:207-246`).

### 3.8 Header writer and between-log records

```c
void blackboxParamsStart(void)          // blackboxStart() (blackbox.c:1169-1210)
{
    bbp.active = (blackboxConfig()->params != BLACKBOX_PARAMS_OFF);
    if (!bbp.active) return;
    clear ring, seq = 0, opDepth = 0, t0 = false, lostMask = 0, header cursor = 0, bbpVerified[] = PRE;
    if (!bbp.shadowValid) { copy live -> shadow (word loop, 2.7 KB, about 8 us); recMarker(PRE, "shadow-reset"); bbp.shadowValid = true; bbp.prevMask = 0; }
    else bbp.prevMask = ALL_TRACKED;    // compared in steps in the header iterations: 'v' records, stamp p
}
void blackboxParamsHeaderTick(void)     // each blackboxUpdate() while the state is WAIT_FOR_READY .. CACHE_FLUSH
{
    if (bbp.active && bbp.prevMask) stepGroups(bbp.prevMask, PRE, 0);    // one step of work (3.7)
}
bool blackboxParamsWriteHeader(void)    // custom case of blackboxWriteSysinfo(); true when param_end is written
{
    if (!bbp.lineReady) { if (!nextHeaderLine(bbpLine)) return !bbp.prevMask; bbp.lineReady = true; }   // T0 waits for the 'v' compare
    const int len = strlen(bbpLine);
    if (blackboxDeviceReserveBufferSpace(len) != BLACKBOX_RESERVE_SUCCESS) return false;   // budget 64 B per iteration, cap 256
    blackboxWriteString(bbpLine); blackboxHeaderBudget -= len;
    bbp.hdrHash = fnv_update(bbp.hdrHash, bbpLine, len); bbp.hdrLines++; bbp.lineReady = false;
    return false;                       // at most one line per call
}
void blackboxParamsRunning(void)        // the state became RUNNING after CACHE_FLUSH
{
    if (!bbp.active) return;
    if (bbp.prevMask) bbpCapture(false, 'v', 0, PRE, bbp.prevMask, -1);    // normally nothing is left
    recRuntimeAll(PRE);                 // one 'R' with all five values: the base for the polls
    bbp.t0 = true;
}
```

- The stages of `nextHeaderLine` are META, then SET, SET@P, SET@R, ELEMENTS, RAW and BOOT for FULL, then END.
- One header line costs at most about 30 µs of formatting. A profile line with 6 values is the largest.

### 3.9 Lifecycle edits in `blackbox.c` and the core

| Site | Change |
|---|---|
| `blackbox.c:334-341`, `:442-448`, `:1085-1113` | the 4 S fields (2.3). `loadSlowState` reads the pointers, `ARMING_FLAG(ARMED)` and `blackboxParamsSeq()`. |
| `:2129` | the field count is `blackboxSlowFieldCount()` (5 or 9) |
| `:1169-1210` `blackboxStart` | `blackboxParamsStart();` before `blackboxSetState(WAIT_FOR_READY)` |
| `:683-711` `blackboxSetState` | cases SHUTTING_DOWN, FULL and START_ERASE: `blackboxParamsStop();` (inactive, ring cleared). Each state that ends a log stops the journal, also when it ends without `LOG_END`. |
| `:1243-1244` | `if (blackboxParamsHoldLogEnd()) break; blackboxParamsEnd();` before `LOG_END`. GRACE goes on logging frames during the hold. PAUSED goes on draining. |
| `:1779` (after `fields_mask`) | `BLACKBOX_PRINT_HEADER_CUSTOM( if (!blackboxParamsWriteHeader()) xmitState.headerIndex--; );` |
| `:1986-1999` `blackboxLogIteration` | `blackboxParamsAfterFrame();` after `writeIntraframe()` / `writeInterframe()` |
| `:2041` `blackboxUpdate` | after `blackboxCheckEnabler`: `if (state >= WAIT_FOR_READY && state <= CACHE_FLUSH) blackboxParamsHeaderTick();` |
| `:2083` | `if (blackboxParamsSkipUartWait() \|\| millis() > xmitState.u.startTime + 100)`: skip the 100 ms wait for FLASH and SDCARD when params are not OFF. The comment at `:2079-2082` says that the wait is for the UART. |
| `:2152-2153` | after `blackboxSetState(cacheFlushNextState)`: `if (blackboxState == BLACKBOX_STATE_RUNNING) blackboxParamsRunning();` (after the assignment at `:712`) |
| `:2156-2172` PAUSED | `blackboxParamsAfterFrame();` before `blackboxAdvanceIterationTimers()`, only when `blackboxLoggedAnyFrames`. In NORMAL mode the log can go from RUNNING to PAUSED before its first frame (the BLACKBOX switch turned off during the header). |
| `:2258` `blackboxInit` | `blackboxParamsInit();` |
| new | `uint32_t blackboxGetIteration(void)`, `bool blackboxIsLogRunning(void)` |
| `blackbox_io.c:665-687` | split out `int32_t blackboxDeviceFreeSpace(void)` |
| `fc/core.c:141` | `uint8_t getPidUpdateCounter(void) { return pidUpdateCounter; }` |
| `fc/core.c:756-846` | first line of each of the 8 `subTask*` functions: `subtaskTick[CORE_ST_x] = pidUpdateCounter;` (one byte store each). `uint8_t coreSubtaskTick(st)` reads it. |
| `fc/init.c:1018` | `blackboxParamsBoot();` before `systemState \|= SYSTEM_STATE_READY` |
| `cli/cli.c:745` `backupConfigs` | `blackboxParamsShadowLost();` |
| `pg/blackbox.h:41-49` | append `uint8_t params;`. The struct grows from 12 B to 16 B with the same PG version, which is safe because `pgLoad` copies `MIN(stored, size)` (`pg/pg.c:76-90`). `MSP_SET_BLACKBOX_CONFIG` (`msp.c:3155-3169`) does not write the field, so a Configurator save keeps it. |
| `pg/blackbox.c:65` | `.params = BLACKBOX_PARAMS_FULL` (fork). The upstream proposal is `CHANGES`. |
| `cli/settings.h`, `cli/settings.c` after `:824` | `TABLE_BLACKBOX_PARAMS {"OFF","CHANGES","FULL"}` and `{ "blackbox_params", VAR_UINT8 \| MASTER_VALUE \| MODE_LOOKUP, ... offsetof(blackboxConfig_t, params) }`. It cannot change while a log is open (`blackbox.c:512-515`). |

### 3.10 Hook table (mutation paths and loaders)

| Path | Site | Call |
|---|---|---|
| All MSP (serial, telemetry, nested) | `msp/msp.c:4074-4102` | Wrap the chain from `:4087` to `:4099` (out-with-arg, passthrough, dataflash read, in-commands) in `bbpOpBegin('m', cmd)` / `bbpOpEnd()`. The two read-only branches (`:4083-4086`) and a skip list do no work. The skip list is SET_RAW_RC, DATAFLASH_READ, SET_PASSTHROUGH, SET_RTC, SET_TX_INFO, SET_RAW_GPS, SET_HEADING, SET_MOTOR, SET_MOTOR_OVERRIDE, SET_SERVO_OVERRIDE(_ALL), SET_MIXER_OVERRIDE, SEND_DSHOT_COMMAND, MULTIPLE_MSP and REBOOT. The bracket includes `MSP_SET_SERVO_CONFIG` (`:2224-2249`) and runs whatever the result is. |
| ESC parameters | `msp.c:3100-3111` after `escCommitParameters()` | `bbpMarker(ESCPARAM, len, fnv)`. The ESC bytes are outside the FC configuration. |
| Adjustments | `fc/rc_adjustments.c:356` | `bbpOpBegin('a', adjFunc); adjConfig->cfgSet(adjval); bbpOpEnd();`. This also catches a set whose read-back equals the stale value, which logs no event today. A set that had no effect (a setter that clamps the value) repeats at each RX frame while the knob stays there. The same set (slot and value) again, with no change of `blackboxParamsChangeCount()` since, has no effect again, so it runs without the operation. The count changes with each change that a capture finds, each loader and each log start. `cfgSet` is called as in 4.6.0. |
| CMS | `cms/cms.c:1021` `cmsHandleKey` (callers `:1354`, `:1373`) | Rename it to `cmsHandleKeyInner`. The wrapper brackets it with `'k'`. CMS opens only while disarmed (`:1466-1468`) and stops arming (`:891`). The log can still be open in GRACE. |
| PID profile switch (sticks, CMS, MSP, adjustment) | `config/config.c:839-851` `changePidProfile` | self-bracketing `'p'` (nested inside MSP, adjustment and CMS operations) |
| Rate profile switch | `fc/rc_rates.c:435-442` | self-bracketing `'p'`, `arg = 0x100 \| index` |
| EEPROM load | `config.c:741-757` `readEEPROM` | self-bracketing `'e'`, then `bbpMarker(EELOAD)` |
| EEPROM save (disarm + 0.5 s dispatch, MSP, CMS, sticks) | `config.c:759-769` `writeUnmodifiedConfigToEEPROM` | self-bracketing `'w'`, then `bbpMarker(EESAVE, us)`. Its `validateAndFixConfig` (`:761`) can change values that no loader applies afterwards. |
| Stick trims | `sensors/acceleration_init.c:451-455` | self-bracketing `'t'` |
| Accelerometer calibration | `acceleration_init.c:410-443` | bracket `'c'` in the first and the last calibration cycle only. The first cycle zeroes `accZero` (`:428`). The last cycle stores it and saves (`:432-440`). |
| Loader `pid` | `flight/pid.c:717` (end of `pidLoadProfile`, after the `pid.dT == 0` return at `:586-587`) | `bbpApplied(BBP_L_PID, slot)`. It also covers leveling, trainer and rescue (`:709-716`). |
| Loader `gov` | `flight/governor.c:1599` (end of the `if (gov.govMode)` block) | `bbpApplied(BBP_L_GOV, slot)`. MSP also calls it directly (`msp.c:3040`). |
| Loader `rsc` | `flight/rescue.c:551` (end of `rescueInitProfile`) | `bbpApplied(BBP_L_RESCUE, slot)` (`msp.c:3016`) |
| Loader `sp` | `flight/setpoint.c:246` (end of `setpointInitProfile`) | `bbpApplied(BBP_L_SETPOINT, rateSlot)` |
| Loader `gyrof` | end of `sensors/gyro_init.c:133` `gyroInitFilters` | (`msp.c:2906`) |
| Loader `rpmf` | `flight/rpm_filter.c:274` (before the success `return`) | (`msp.c:2909`, `:2925`) |
| Loader `mix` | `flight/mixer.c:642` | (`msp.c:3454`) |
| Loader `rcctl` | `fc/rc_controls.c:376` | (`config.c:159`, `msp.c:2597`) |
| Loader `act` | `config/config.c:173` (end of `activateConfig`) | `initRcProcessing`, `adjustmentRangeInit`, `failsafeReset`, `accInitFilters`, `imuConfigure` (`:149-173`) |
| Loader `feat` | `config/feature.c:37` (end of `featureInit`) | the runtime feature mask (`:32-37`) |
| Fingerprint accessors | `pid.c`, `governor.c`, `setpoint.c`, `feature.c` | `pidParamFingerprint()`, `governorParamFingerprint()`, `governorRuntimeMode()`, `setpointParamFingerprint()`, `featureRuntimeMask()` |

**Completeness while armed.** The paths that write configuration while a log is open are these:
- MSP, through one entry for all transports;
- adjustments;
- profile changes;
- EEPROM load and save;
- CMS, stick trims and calibration, which are disarmed only (CMS at `cms.c:1466-1468`, sticks at `rc_controls.c:192`);
- `statsOnDisarm`, which writes an untracked PG.

The CLI stops the blackbox (`core.c:830`). The audit in 6.2 lists the remaining writers of tracked PGs. They are boot-only (`servos.c:190`, `gyro_init.c:544-568`, `esc_sensor.c:4298-4303`) or are inside an operation:
- the adjustment setters (`adj_setters.txt`);
- `changeBatteryProfile` (`sensors/battery.c:251-256`);
- `featureEnable/DisableImmediate`, called only from `validateAndFixConfig` (`config.c:227-330`).

The scan catches anything that the audit missed, as a `u` record with an interval.

### 3.11 Behaviour fixes (PR-C; separate commits, optional, listed in `param_fixes`)

| Id | Site | Fix |
|---|---|---|
| c1 | `msp.c:2851-2853` `MSP_SET_RESET_CURR_PID` | add `pidLoadProfile(currentPidProfile);`, as `SET_PID_TUNING` does at `:2577` |
| c2 | `msp.c:2503-2517` `MSP_COPY_PROFILE` | reload the destination when it is the active profile, also while armed (consistent with every other armed MSP set). In the rate branch compare with `getCurrentControlRateProfileIndex()`, not the PID index (`:2515`). |
| c3 | `setpoint.c:96-129` setpoint-boost setters | also set `sp.boostGain[axis] = value * SP_BOOST_SCALE;` (as `:225`) |
| c4 | `rc_rates.c:71-163` rc-rate, s-rate and expo setters | call `setpointInitProfile()` after the write, so that `sp.ringLimit` (`setpoint.c:237-240`) follows. `SET_RC_TUNING` already does this in flight through `loadControlRateProfile` (`msp.c:2656`). |
| c5 | `msp.c:3545-3553` `MSP_SET_RC_CONFIG` | add `initRcProcessing();` (`rc.c:226`) |
| c6 | `rc_rates.c:430` | `currentControlRateLimits = &ratesSettingLimits[currentControlRateProfile->rates_type];`. Today the table is indexed by the profile index, while `config.c:186-190` uses the rates type, so the rate adjustments clamp with the wrong limits. The journal records the stored value exactly either way. |

The journal does not depend on PR-C. Without PR-C, these cases show as pending in the viewer (4.3).

### 3.12 Budgets

Times are estimates from instruction counts. The bench test (6.4) measures them.

| Item | G474 (Fireball, 170 MHz, blocking W25N writes) | F722 (Gaui, 216 MHz) |
|---|---|---|
| RAM, static | about 2.8 KB (10,336 B free before, about 7.5 KB after) | about 2.8 KB (DTCM 24,936 B free, SRAM1 89,612 B free) |
| RAM with `BBP_OWN_SHADOW` | +2,683 B (about 4.8 KB stays free) | +2,699 B |
| Flash | 9-12 KB (the prototype module measured +5,172 B: FLASH1 442,356 to 447,528 B) of 57,356 B free | 9-12 KB of 38,098 B free |
| Full compare of 2,679 B (unrolled word loop, 1.5-2 cycles/B) | 16-32 µs | 13-25 µs |
| One write operation (pre-capture, 1-3 regional loader captures, post-capture, items) in the MSP, RX or CMS task | 45-90 µs, once per human action | 35-70 µs |
| Idle, per logged iteration (scan of 128 B, plus one poll every 4 iterations) | about 1-2 µs (0.1-0.2 % at 1 kHz) | about 1-1.5 µs (0.2-0.3 % at 2 kHz) |
| Drain while records wait (format one event of at most 128 chars) | 5-15 µs, 1-30 iterations per change | 4-12 µs |
| One capture step in the PID task (`v` compare in the header, resync after a loss, `u` after a scan hit; 3.7) | 10 µs or less (128 units) | 8 µs or less |
| Header line (header states only, at most one per call) | 30 µs or less | 25 µs or less |
| Extra flash bytes in one flush during a drain | 131 B or less, so 25 µs or less of blocking SPI at about 0.19 µs/B | non-blocking path |
| Header bytes | FULL about 15 KB, CHANGES about 0.6 KB | same |
| S-frame | +2-6 B for each S-frame | same |
| Journal | 30-130 B per change. A profile switch is 2-3 events, about 200 B. | same |
| Blind window (arm to first I-frame; 4.6.0: 222-229 ms Fireball, 155-163 ms Gaui) | FULL at 64 B/iteration adds about 234 ms. The skipped UART wait saves 100 ms. Net about 360 ms. CHANGES: about 130 ms. | FULL adds about 117 ms, minus 100 ms: net about 180 ms. CHANGES: about 65 ms. |
| Blind window, serial logger below 1 Mbaud (`blackbox_io.c:327-337`: about 6000 B/s, also 4000-8000 B/s at 4-8 kHz; the UART wait stays) | FULL would add about 2.3 s (13.8 KB at 6000 B/s), so `blackboxParamsStart()` uses CHANGES: about 0.1 s | same |
| `LOG_END` | delayed by 100 ms or less while records wait | same |

- **Re-arms in GRACE** have no header and no blind window, as in 4.6.0.
- **Option.** A compile-time `BBP_HEADER_BYTES_PER_ITERATION 128` for FLASH halves the added header time, but doubles the SPI time per header tick on G4. Enable it only after the bench test.
- **Upstream concern.** An upstream objection to the FULL blind window is answered by the default CHANGES. The fork default is FULL: on flash and SD the blind window is above, and a serial logger below 1 Mbaud gets CHANGES.

### 3.13 Upstream split

1. **PR-A** (about 60 lines): the S-frame fields `pidProfile`, `rateProfile` and `armed`, present unless OFF. They give every frame its profile index in any explorer, with no decoder change.
2. **PR-B**: the module. It adds the header section, `paramSeq` and the journal, with the toggle (upstream default CHANGES).
3. **PR-C**: c1-c6, each as its own commit.
4. **PR-D**: skip the UART wait for FLASH and SDCARD.

---

## 4. Viewer changes (`/Users/albertwu/exp/rotorflight-blackbox`)

### 4.1 Decoder and display

- **Header lines.** In `js/flightlog_parser.js:1068-1073` (the default branch of the header parser), keys `Param log`, `param_*`, `set.*`, `set@p*`, `set@r*`, `el.*`, `pg.*` and `boot.*` go into `sysConfig.paramHeader`, an array of `[name, value]` that each log owns. They do not go into `unknownHeaders` and do not print a console line. Event 101 and the extra S fields already decode, and frames carry `pidProfile`, `rateProfile`, `armed` and `paramSeq`.
- **Events before the first I-frame.** In `js/flightlog_index.js:157-160` and `js/flightlog.js:330`, chunk 0 must start at the end of the header, not at `iframeDirectory.offsets[0]`. It keeps events before the first I-frame and gives them the time of the first frame. This also fixes the SYNC_BEEP and GOVSTATE events that are lost today.
- **Header dialog.** `js/header_dialog.js:458-460` must escape `& < > " '` in names and values. Today it builds raw HTML. The `paramHeader` lines go in their own collapsible table.
- **Graph labels.** `js/grapher.js:578-579`:
  - draw one marker per `C` record (or one per group of records at the same point), with STE text;
  - put names in `<code>` or `data-ste="quoted"`;
  - do not draw `A`, `R`, `M`, `L`, `Q` or `+` strings;
  - draw other event-101 strings as today.

### 4.2 New `tools/autotune/param_log.cjs`

It is worker-loadable CommonJS: no Node API at load, then `module.exports`, then `if (require.main !== module) return`. It needs a `distSources` entry in `gulpfile.js` (`:294`) and an entry in the worker module list.

```js
const RULES = { eventMax: 128, rxTransitionFrames: 'one rcCommand period', ... };   // thresholds live here
isParamLog(sysConfig) -> boolean
parseParamHeader(paramHeader) -> { version, mode, loop, phase, pgs, fixes, rt, pid0, rate0,
                                   master: Map, pid: Map(name -> text[6]), rate: Map, el: Map, raw: Map, boot: Map,
                                   end: { lines, hash }, complete }          // complete: param_end count and FNV-1 agree
parseRecord(text) -> null | { type, seq, at: { pre } | { n, c } | { n0, c0, n1, c1 }, src, arg, n, items: [{ key, value, old }], crcOk }
assemble(strings) -> { records: Map(seq -> record), gaps: [...], crcErrors }
buildTimeline({ header, records, frames, semantics }) -> Timeline
```

- **`frames`.** Typed arrays per frame, built from `getChunksInTimeRange` (never the smoothed variant): `iter` (`loopIteration`), `time` (µs), `pidProfile`, `rateProfile`, `armed` and `paramSeq`.
- **The timeline.**

```js
Timeline = {
  complete,                              // snapshot complete and no losses
  t0State,                               // Map(key -> text)
  epochs: [{ i0, i1, t0, t1, pidProfile, rateProfile, armed, seq, status, pending: [key], unknown: [key], reasons: [] }],
  mixed: Set(iter), uncertain: [{ key, i0, i1 }],
  valueAt(key, iter, consumer) -> { stored, running, status: 'exact'|'mixed'|'uncertain'|'unknown'|'pending'|'effect-unknown', source: 'header'|'record'|'boot' },
  stateAt(iter) -> Map(key -> stored text), plus running overlay
  classic(iter) -> classic sysConfig keys (4.4)
}
```

- **The algorithm** is the fold of 2.6.
- **Epochs.** An epoch boundary falls at:
  - each `C`, `A` and `R` point, and each interval end;
  - each change of `pidProfile`, `rateProfile` or `armed`;
  - each `LOGGING_RESUME`.

### 4.3 New `tools/autotune/param_semantics.cjs` (what is in force)

`forFirmware(revision, fixes) -> table`. The table for "Rotorflight 4.6.*" has these parts:

- **`loaders`.** For each `A` loader: the keys that it reads (the same PGs as 3.6) and the slot rule (the PID slot or the rate slot).
- **`keys`.** An ordered list of `{ match, use: [{ by: 'live'|'loader'|'boot', loader, consumer, tick|task }], src }`. Seed for 4.6.0, with the evidence:

| Keys | Use | Evidence |
|---|---|---|
| PID-profile gains, filter cutoffs, offsets, relax, precomp, cross-coupling | loader `pid` | `pid.c:583-717` |
| governor profile keys | loader `gov` (or `pid`) | `governor.c:1543-1601`; `pid.c:709` |
| rescue profile keys | loader `rsc` (or `pid`) | `rescue.c:525-551`; `pid.c:716` |
| leveling and trainer keys | loader `pid` | `pid.c:711`, `:714` |
| rate curve keys (`rates_type`, rc-rate, s-rate, expo) | `live` for the setpoint curve, plus loader `sp` for the cyclic ring limit (two consumers) | `setpoint.c:237-240` |
| response, acceleration limit, boost, yaw dynamic, cyclic ring, polar | loader `sp` | `setpoint.c:207-246` |
| GOVERNOR_CONFIG | `boot` | `governor.c:1611-1672`, `init.c:799` |
| `debug_mode` | `boot` | `init.c:411` |
| `pid_process_denom`, `filter_process_denom` | `boot` | `init.c:685-691` |
| servo `rate` | `boot` | `servos.c:139-190` |
| motor poles, gear ratios | `boot` | `motors.c` `rpmSourceInit` (`init.c:784`) |
| gyro LPF and notch | loader `gyrof`; `gyro_hardware_lpf` is `boot` | |
| RPM filter | loader `rpmf` | |
| mixer config | loader `mix` | |
| `rc_center` | `live`, RX task | `rc.c:222` |
| deadband, deflection, throttle limits | loader `act` (`initRcProcessing`), RX task | `rc.c:226-246` |
| accelerometer trims | `live`, ACC task | `tasks.c:164` |
| features | loader `feat` for the runtime mask; the subsystems are `boot` | `feature.c:32-37` |
| anything else | `effect-unknown` | |

- **`adjustments`.** Function id to the runtime fields that its setter refreshes, from `scratchpad/adj_setters.txt`. A setter can refresh more than one field. Examples:
  - 76 and 77 refresh `gov_idle_throttle` and `gov_auto_throttle` (`governor.c:1359-1377`);
  - 68-71 (setpoint boost) refresh nothing (`setpoint.c:96-129`);
  - 72-74 (yaw dynamic) refresh their runtime fields (`setpoint.c:136-160`);
  - 5-13 (rates) refresh nothing, because the curve is live and the ring limit is cached;
  - 1 and 2 are profile changes, which bring their own `A` records.

  With `param_fixes` c3 and c4, the 68-71 and 5-13 entries change as section 3.11 says.

**Running value of key K for consumer X at a point.**
1. **`live`:** the stored value at the tick of X.
2. **`loader L`:** the stored value at the last `A(L)` with the same slot before the point. A `C(K)` after that `A` makes K **pending** for X.
3. **`boot`:** the `boot.*` value if one exists, else the snapshot value.
4. **Adjustment refresh:** the stored value at the adjustment record, for the refreshed fields.
5. **Unknown:** the stored value with the flag `effect-unknown`.
6. **Fingerprint check.**
   - Each `R fp.X` change must have, inside its interval, an `A` of module X or a refreshing adjustment. If not, the flag is `unexplained-runtime`.
   - Each `A` of module X that follows a `C` of a key that X reads, and whose `fp` does not change, gets the flag `table-suspect`.
   - Both flags prove an error in the table without a reflash. The table can be fixed, and old logs can be read again.

### 4.4 Mapping to the classic `sysConfig` keys

`classic(iter)` builds the keys of `blackbox.c:1628-1720` from the state, for the PID slot and the rate slot of that frame:

- `rollPID`, `pitchPID`, `yawPID` = `[<axis>_p_gain, _i_gain, _d_gain, _f_gain, _b_gain]` (`:1649-1663`)
- `govPID` = `[gov_p_gain, gov_i_gain, gov_d_gain, gov_f_gain, gov_gain]` (`:1668-1672`)
- `rc_rates` = `[roll_rc_rate, pitch_rc_rate, yaw_rc_rate]`, `rc_expo` = `[roll_expo, ...]`, `rates` = `[roll_srate, ...]`, `response_time`, `accel_limit`
- the remaining keys in the order of `:1673-1720`

**Self-test.** In each param log, `classic()` at the point where the classic lines were written must equal the classic `H` lines (both come from the same RAM). A unit test checks this on every fixture.

### 4.5 `tools/autotune/param_epochs.cjs` (agreed contract kept)

`paramEpochs(input)` returns `[{ t0, t1, arm, armed, pidProfile, rateProfile, fresh, reasons, adjust, check }]` (`param_epochs.cjs:25-38`).

- **New optional input `timeline`.** When it is present:
  - the spans are `timeline.epochs`;
  - `pidProfile` and `rateProfile` come from the S-frame, 1-based (the app writes "PID profile 1" to "PID profile 6");
  - `arm` counts the 0→1 changes of the S-frame `armed`;
  - `check` is `'journal'`;
  - `adjust` lists the `C` records of the span;
  - `fresh` means that `reasons` is empty.
  - **New reasons:** `mixed`, `uncertain`, `pending`, `unknown`, `effect-unknown`, `lost`, `snapshot-incomplete`, `chain-mismatch`, `order-unknown`, `unexplained-runtime`, and `resume` (kept).
  - The old reasons `grace`, `rearm`, `switched` and `unlogged` do not occur for a param log.
  - **Added fields:** `seq`, `status` and `keys` (the unknown and pending keys).
- **Without `timeline`** (stock logs): the current inference, plus the detectors of 5.3.

### 4.6 Consumers

- **`tools/autotune/datasets.cjs`.**
  - At `:468-472` (the configuration identity), exclude `paramHeader`, so that `param_*` lines that differ for each log (`param_rt`, `param_pid_profile`) do not split configurations.
  - For a param log, build the configurations per journal epoch from `timeline.stateAt()`, with `source: 'journal'`.
  - Remove mixed frames, uncertain frames and transition frames from the samples, for the keys involved.
- **`tools/autotune/advice.cjs`.**
  - The "from" value of a recommendation is `valueAt(key, iter, consumer).running` for the PID profile and the period of the result.
  - If the stored value differs from the running value (pending), show both and give no CLI text for that key.
  - If the profile is not known, show "PID profile unknown", with no CLI text and no export (CLAUDE.md).
- **`js/tuning_worker.js:338-346`.** Also collect the event-101 strings that start with `P[CARMLQ+]`, not only `INFLIGHT_ADJUSTMENT`. Build the timeline once per log and pass it to `param_epochs`, `datasets` and `advice`.
- **Log lens.** Add a "Parameters" panel at the cursor. It shows the PID and rate profile, the records of the epoch, the pending and unknown keys, and the status.
- **UI text** must be ASD-STE100 and pass `node --test test/ste_text.test.cjs`. These candidate texts need the lint:
  - "The log records the parameter values for this period."
  - "The log does not record all the parameter values for this period."
  - "The flight controller has this value in its configuration. It uses the value only after it starts again."
  - "The value changed during this frame. The analysis does not use this frame."
- **Gear ratios.** Show motor poles and gear ratios only as logged values, with no judgement (CLAUDE.md, gear-ratio rule).

### 4.7 Packaging, rules, tests

- **`gulpfile.js` `distSources`.** Add `tools/autotune/param_log.cjs` and `tools/autotune/param_semantics.cjs`, and add both to the CommonJS shim list of `js/tuning_worker.js`.
- **CLAUDE.md, the "from"-value rule.** Change it to four sources:
  - (1) the parameter journal of the log (header snapshot and records). This source has priority and is valid for every PID profile and period of a param log.
  - (2) the CLI section of that PID profile;
  - (3) the gains that the analysis calculates;
  - (4) the classic log header, for stock logs only, and only if its PID profile was active at the arm.
- **Tests.**
  - `test/helpers/bbl_encode.cjs`: add event 101 (`log.strings: [[frame, text]]`), the 4 S fields, and `paramHeader` lines. `EV` at `:29` gets `CUSTOM_STRING: 101`.
  - `test/param_log.test.cjs`: header parse and hash; CRC; continuations; gaps; the trailing gap from `paramSeq`; `L` and `y`; `u` intervals; mixed frames for denominators 1-4 and 8; pending and boot values; fingerprint flags; `classic()` against the classic lines; and the stock fallback.
  - Fixtures are the golden outputs of the firmware host test (6.1).
  - Run `test/viewer_regression.test.cjs` and the CLAUDE.md suites without change on stock logs.

---

## 5. Interim measures for stock 4.6.0

### 5.1 Configuration

```
# both helicopters
set blackbox_grace_period = 1        # cli/settings.c:824, range 0-60
set stats_min_armed_time_s = -1      # STATS_OFF (pg/stats.h:26): statsOnDisarm saves nothing (fc/stats.c:58-61)
# Fireball only: adjfunc 0 2 255 1500 1500 1 1005 2000 1500 1500 0 1 3 selects PID profile 1-3 from AUX2
# (channel index 1, continuous; fc/rc_adjustments.c:336-348 gives 1 below 1254 us, 2 from 1254 to 1751 us, 3 from 1752 us).
# Mirror the switch into USER1-3: permanent ids 40-42 (msp/msp_box.c:90-92), box ids 28-30 < 32, so they are in the
# S-frame flightModeFlags (blackbox.c:1108). Use three unused aux slots (check the 'aux' output first); 17-19 here.
aux 17 40 1 875 1250 0 0
aux 18 41 1 1255 1750 0 0
aux 19 42 1 1755 2125 0 0
save
```

Costs and reasons:
- **Grace period 1 s.**
  - Re-arm gaps in the data: minimum 0.003 s, median 0.442 s, 90th percentile 2.223 s, maximum 3.518 s (Fireball) and 4.161 s (Gaui).
  - With 1 s, each re-arm that leaves time for a Lua edit gets its own log and a new header.
  - Re-arms of less than 1 s stay in one log with no blind window. These are about half of the re-arms, and they include blips and in-flight recoveries.
  - The disarm + 0.5 s save stays inside the log. The detector of 5.3 needs it.
  - Data after a disarm is limited to 1 s.
  - **Not 0:** 0 loses the save marker and all data after the disarm, adds 155-229 ms with no data after each arm, and makes header-only logs for arm blips (5 of 8 cycles in `0929#18` are shorter than 0.2 s).
  - Neither value makes armed MSP writes, the profile index or the 224 missing settings visible.
- **`stats_min_armed_time_s = -1`.** After this, the disarm + 0.5 s save (`core.c:452-460`) happens only when `configIsDirty` is set:
  - by an adjustment (`rc_adjustments.c:368`), which logs an event;
  - by `MSP_EEPROM_WRITE` while armed (`msp.c:3133-3136`).

  Thus a save stall in an arm cycle with no `INFLIGHT_ADJUSTMENT` proves a Lua or Configurator save while armed. The cost is that the flight, time and distance counters stop.
- **USER box mirror.**
  - USER boxes have no effect while the PINIO box configuration is the default (`pg/piniobox.c:36-42`, `msp/msp_box.c:255-266`).
  - The mirror shows the switch position. It does not show an `MSP_SELECT_SETTING` override, and an adjustment acts only when the switch value changes.
  - It does not show the rate profile. Positions between the bands show no USER bit.
  - Test it once on the bench, without props.

### 5.2 Pilot practice

- Do not press Save or select a profile in the Lua or the Configurator while armed, or less than 2 s after a disarm. The RF2 Lua does not check the arm state.
- To change the PID profile in flight, use only the switch. The switch writes `INFLIGHT_ADJUSTMENT` events and the USER bits.
- Keep a different `gov_headspeed` for each profile, so that `govRequest` identifies the profile in governed flight.
- Reboot after you save governor, motor, filter or `debug_mode` changes. The Lua reboots after Filters and Governor saves.
- Save `diff all` at the start of each session and after each edit session. Its `profile N` and `rateprofile N` lines give the active indices.
- Optional: patch the RF2 Lua so that it refuses Save and profile selection while armed. This closes the armed-write path of the Lua only.

### 5.3 What the viewer can say about stock logs

Stock logs never become exact.
- Every span gets `check` from `recoverGains` (gains recovered to ±0.1 count): `verified`, `step` or `untestable`.
- A span is "PID profile unknown" unless one of these confirms it: an `INFLIGHT_ADJUSTMENT` func 2, a USER bit, a unique `govRequest` plateau, or a unique match of the header gains with a section of a CLI dump.

New detectors in `param_epochs.cjs`, each with a `RULES` entry:
- **`savedWhileArmed`.** A frame gap of 20-35 ms (F7) or 60-85 ms (G474) at disarm + 0.50 ± 0.03 s, in an arm cycle with no `INFLIGHT_ADJUSTMENT`, and with stats off. The reason is `saved-armed`, and the values of that arm cycle are unknown.
- **`userBits`.** The S-frame `flightModeFlags` bits 28-30 give the PID profile 1-3 for each frame. They agree with func-2 events when both exist.
- **`rebootBoundary`.** The uptime goes down between consecutive logs. Boot-cached values can differ only across such a boundary.
- **`graceSave`.** A save stall in GRACE that is not at +0.5 s. The values of the next re-arm are unknown.

---

## 6. Verification plan

### 6.1 Host and unit tests

1. **Port the prototype harness.** Port `design/minimal-diff/hosttest/harness.c` and its PG shim (it has golden header and record files). Extend it to the new module. The checks:
   - the formatter equals CLI `printValuePointer` for every `valueTable` entry with random bytes;
   - lookups out of range, bitsets, int8, uint32, arrays and strings with `%XX`;
   - the word diff and the bitmap walk: one bit of a bitset, one element of an array, profile strides, elements, raw remainders;
   - ring overflow gives a committed partial record, `L`, then `y` with an interval, with no stolen exact stamp;
   - the pre-capture: a planted unhooked write before an operation becomes a `u` record with interval `[verified, pre]`, and the operation's own write keeps its exact point;
   - the stamp rule for every denominator 1-16 and every `c` in `0..denom-1`, against a model of `core.c:885-1056`;
   - before T0: `p` stamps, `v` records and `M shadow-reset`;
   - operation nesting (MSP, then `changePidProfile`, then `pidLoadProfile` markers): one source, `C` before `A`;
   - the header: the line budget, the empty-field compression, the fallback of lines longer than 192 chars, and the `param_end` count and hash;
   - events: 128 chars or less, CRC, continuations;
   - the scan: `bbpVerified` is the start of the pass.
2. **gtest.** Add `src/test/unit/blackbox_params_unittest.cc` (the RF blackbox and CLI unit tests are disabled as `.txt`). Build on Linux or with the repository Docker image. On macOS the SITL and unit builds fail: clang rejects `-fuse-linker-plugin` (`build_sitl_base.log`), and gcc 14 stops on warnings in `target/SITL/target.c` (`build_sitl_gcc14b.log`).
3. **Builds.** Build STM32G47X, STM32F7X2, STM32F405 and STM32F411. Record the RAM and flash from the map files. Verify that the PG `copy` buffers exist for all tracked PGs (`nm`).

### 6.2 Static audit (on every firmware update)

- List every write to a tracked PG outside `init()`: grep `Mutable(`, `memcpy` into PG RAM, and `currentPidProfile->` and `currentControlRateProfile->` assignments. Each one must be inside an operation, or must be listed as boot-only.
- For each loader of 3.6: grep the PGs that its body and callees read. The region table must cover them. The viewer `loaders` table must name the same PGs.
- Assert that `->copy` is used only in `cli.c` and `blackbox_params.c`.
- Re-derive the adjustment refresh table from the setters, as `adj_setters.txt` does.

### 6.3 SITL end to end

**Setup.**
- `make TARGET=SITL` on Linux.
- A Python FDM stub sends a constant rate on UDP 9003 (`target/SITL/target.c:234-237`; README).
- UART1 on `tcp://127.0.0.1:5761` is the MSP client. UART2 on `:5762` is `blackbox_device = SERIAL`, at the highest baud that the CLI accepts, captured with `nc`.
- Arm through MSP RX (`MSP_SET_RAW_RC`).
- `gov_mode = OFF` or DIRECT, because without an RPM source ELECTRIC and NITRO block arming (`governor.c:1625-1633`).
- The client keeps a ground-truth log of each write with the `MSP_STATUS` cycle time.

**Scenarios.**
1. `SET_PID_TUNING` while armed.
2. `SELECT_SETTING` while armed.
3. `COPY_PROFILE` onto the active profile while armed (pending without c2).
4. `SET_RESET_CURR_PID` (pending without c1).
5. `SET_GOVERNOR_CONFIG` while armed (boot-cached, pending).
6. `SET_SERVO_CONFIG` while armed (out-with-arg branch).
7. `SET_FILTER_CONFIG` (`A gyrof`, `A rpmf`).
8. An adjustment through an MSP aux channel: profile, gain, and setpoint boost (pending without c3).
9. Disarm, a write in GRACE, and a re-arm within 1 s.
10. `EEPROM_WRITE` in GRACE (`e`, `w`, `M` records).
11. Writes during the header (`p` stamps).
12. A slow port that fills the ring: `L` and `y`.
13. Many writes between two logs (`v` records).
14. `kill -9` in the middle of a log (trailing gap).
15. CLI entry in GRACE (truncated log, gap detected).
16. `pid_process_denom` 1, 2, 3, 4 and 8.
17. A test-only writer that bypasses the hooks (`u` record).

**Pass criteria.**
- Each ground-truth write has a `C` with the correct point. With a constant gyro error e and a known stick, the logged `axisP` equals `Kp_scale × P × e` from the first frame after the point. The mixed frame matches the tick rule.
- There are no `u` or `y` records except in scenarios 12 and 17.
- `valueAt` equals the ground truth at every frame outside the flagged frames.
- The last state equals `diff all`.
- The viewer decodes with no corrupt frames.
- Old explorers open the log.

### 6.4 Hardware bench without props (Fireball G474, Gaui F7, real flash)

**Steps.**
- Run each of these 10 times:
  - a Lua yaw P ±1 save while armed;
  - a Lua profile selection while armed;
  - an adjustment-switch profile change;
  - a disarm and a re-arm within 1 s;
  - an EEPROM save;
  - CMS in GRACE;
  - CLI entry in GRACE.
- Do one power pull while the log records.

**Measurements.**
- Arm to first I-frame (blind window), against a stock build. Also with a serial logger at 115200 baud (FULL gives CHANGES) and at 2 Mbaud.
- `debug_mode = SCHEDULER_DETERMINISM` and `PIDLOOP`: late-task counts and the cycle-time distribution.
- A test-only drop counter in `flashfsWriteByte` (`io/flashfs.c:576-590`).
- The operation time, with the cycle counter around `bbpOpBegin` and `bbpOpEnd`.
- The drain time.
- The slowest `blackboxParamsHeaderTick()`, `blackboxParamsWriteHeader()` and `blackboxParamsAfterFrame()` call, with the cycle counter, in the worst cases of the unit test `WorstPidTaskCall`: every value of the 6 PID profiles and the 6 rate profiles changed (a) between two logs, (b) by an operation that overflows the ring, (c) by a write that no hook saw. On the host (-O2) the slowest capture step is about 0.2 µs, and the slowest call with a drain of a 128-char event about 1.1 µs. The slowest header line is about 2 µs.
- The stack high-water mark.
- The `tasks` CPU.

### 6.5 Viewer tests

`node --test` with `test/param_log.test.cjs` and the CLAUDE.md suites. Run `AUTOTUNE_REAL_LOG` with the real stock logs (no change in results), plus the SITL and bench logs as fixtures.

### 6.6 Release gates

- The flashfs drop counter is 0.
- The late PID tasks and the 99.9th percentile of the frame interval are the same as stock within noise, apart from the known EEPROM stalls.
- An operation takes 100 µs or less on G474 and 80 µs or less on F7.
- The idle cost in the PID task is 3 µs or less per logged iteration.
- The slowest PID-task call of the journal (6.4) is within its 3.12 budget on G474 and F7.
- The blind window is 4.6.0 + 160 ms or less (Fireball) and + 40 ms or less (Gaui) in FULL.
- SITL meets all criteria of 6.3.
- No flight before the bench gates pass.

---

## 7. Residual inexactness

1. **Stored versus running.**
   - The log gives the stored bytes, the stamped loader runs, the boot values and fingerprints of the pid, governor and setpoint runtimes.
   - The value that a computation used comes from the viewer's semantics table for that firmware.
   - A key that the table does not classify is shown as `effect-unknown`, never assumed.
   - Errors in the table are detected only for the fingerprinted modules. They are silent for other cached copies: RX processing, failsafe, IMU, accelerometer filters, mixer, RPM source and servo PWM.
2. **Mixed frames.** When `pid_process_denom > 1` (2 on both helicopters), at most one frame per change is mixed when the change falls between ticks. The log gives the split exactly, and the analysis drops that frame.
3. **Consumers outside the realtime block.** A change takes effect in the RX, ACC and other tasks at their next run after the point. These frames are marked as transition frames for the keys involved.
4. **Unattributed writes.** A write that no hook saw is exact in value, but its time is known only within an interval. The interval is from the last verified point to the detection, and is at most one scan pass (2,679 B / 128 B = 21 logged iterations). The design target is zero, and SITL asserts it.
5. **Losses.**
   - A ring overflow gives `L`, then `y` with an interval.
   - A power cut, a CLI entry, an MSP reboot or a flash fault before the drain leaves the keys unknown from the earliest point of the lost records. Each key becomes exact again at its next record.
   - A `LOG_END` hold that expires gives `Q unsent > 0`.
   - All losses are detected (SEQ, `paramSeq`, CRC, `Q`). None is silent.
6. **A cut header.** An arm blip or a disarm during the header gives no `param_end`. The snapshot is then incomplete, but such logs have few or no frames.
7. **Merges inside one operation.** Writes inside one operation get one point. An A→B→A inside one MSP command is not visible. It also has no effect on any frame, because no PID tick runs inside a task.
8. **Order inside an operation.** A loader region that misses a PG gives `order-unknown` for the keys written before the loader in the same operation (3.6).
9. **Not tracked:**
   - ESC-internal parameters (only `M escparam` with its length and hash);
   - radio curves and mixes;
   - mechanics;
   - the excluded PGs (adjustment ranges and aux modes, whose effects are logged; LED, OSD, telemetry, serial, pins, stats);
   - servo, motor and mixer overrides (in force only while disarmed);
   - compile-time constants, identified only by the firmware revision line.
10. **Integers, not coefficients.** The log gives PG integers, not the scaled or clamped floats, for example `constrain(iterm_relax_cutoff, 1, 100)` or the `*_TERM_SCALE` factors. The viewer applies the scaling from the firmware source when it needs a float.
11. **Data, not parameters.** The `axisB` and `axisO` fields exist only if the profile at log start had B or O greater than 0 (`blackbox.c:553-562`).
12. **Stock 4.6.0 logs** are never exact (5.3).

---

## Appendix A. Judge findings: verification and resolution

| # | Finding (design, judge) | Verified at | Resolution |
|---|---|---|---|
| A1 | "source adj means in force" is false for the setpoint-boost setters (resource-compat; exactness) | `setpoint.c:96-129` write only the profile; `sp.boostGain` is set only at `:225` | 4.3: no source rule; `A` records and the table; c3 |
| A2 | Rate adjustments leave `sp.ringLimit` stale (minimal, resource-compat; exactness) | `rc_rates.c:71-163`; `setpoint.c:237-240` | two consumers in the table; c4 |
| A3 | Idle/auto adjustments refresh both runtime fields (minimal and PJ; exactness) | `governor.c:1359-1377` | the adjustment table holds a set of fields |
| A4 | The disarm save fixes values with no reload (minimal; exactness) | `core.c:459` → `config.c:781-785` → `:759-769` (`validateAndFixConfig` at `:761`, no `activateConfig`) | `w` operation; pending in the viewer |
| A5 | No `p > b` exception; filter task ignored (resource-compat; exactness) | `core.c:915-930` (denominator 3: BB at tick 1, filter at tick 2); `core.c:856-859` | 2.5, 3.5 |
| A6 | Scan bound in logged iterations (resource-compat; exactness) | the drain runs only on logged frames (`blackbox.c:1986-1999`) | per-PG verified points (3.4, 3.7) |
| A7 | Overflow leftovers stolen by a later hook (resource-compat; exactness) | design text | `lostMask`, `y` records, pre-capture |
| A8 | Header-window captures stamped `0.c` (all designs; exactness) | `blackbox.c:1161-1164`, `:2171`, `:2185` | `p` stamps (3.5) |
| A9 | Apply bits credited to an unrelated capture (resource-compat; exactness) | design text | stamped `A` records after a regional capture |
| A10 | MSP hook misses the out-with-arg branch (minimal; exactness, compatibility) | `msp.c:2224-2249` reached from `:4087`; the hook in the patch is in the final else only | bracket all but `:4083-4086` (3.10) |
| A11 | A deferred compare stamps unhooked changes with the next trigger (minimal; exactness) and hooks claim differences that they did not cause (PJ; exactness) | design text | pre-capture in every operation (3.6) |
| A12 | No per-frame carrier, no end record, no CRC; tail loss is silent (minimal; exactness) | `flashfs.c:576-590` | `paramSeq`, `Q`, CRC-8 |
| A13 | `cli.prev` claimed to survive a reboot (minimal; exactness, compatibility) | the shadow is set at every boot (`init.c:1018` here) | `v` = "since the end of the previous log of this boot, or since boot" |
| A14 | Boot values need another log; rolling erase is on (minimal; exactness, compatibility) | `pg/blackbox.c:64`, `target.h:91`, `flashfs.c:371-390` | `boot.*` lines in every log. A log whose header is erased cannot be decoded at all (no field definitions). |
| A15 | Records up to 255 chars; the decoder limit is 252 (minimal; compatibility) | `flightlog_parser.js:16`, `:1788` | 128 chars or less |
| A16 | PID-task diff of 200-265 µs, unbounded `LOG_END` drain, 4.87 KB nano copy (minimal; safety) | `Makefile:300` nano.specs (judge); `blackbox.c:1243-1244` | capture in the changer task with a word diff; one event per iteration; hold of 100 ms or less; no bulk copy |
| A17 | B, P and R at T0 dropped by the state gate (PJ; exactness, compatibility) | `blackbox.c:683-711` assigns the state at `:712`; the gate is at `:1797-1802` | nothing before RUNNING; T0 hook after `:2152-2153` |
| A18 | 38 KB streamed after T0; 128 B reserve; compare on every MSP command; no runtime OFF (PJ; safety) | `flash_w25n.c:38-42`; `scheduler.c:665` (serial never deferred) | snapshot before T0; 192 B reserve; no work for reads and the skip list; toggle |
| A19 | "A CLI setting needs a PG layout change" is false (PJ; compatibility) | `pg.c:76-90` | toggle appended (3.9) |
| A20 | Hook capture about 150-200 µs and `schedulerIgnoreTaskExecTime` in non-realtime tasks (resource-compat; safety) | `scheduler.c:296-308`, `:650-668` | bitmap walk; the call removed |
| A21 | 2.7 KB copy at `blackboxStart` (resource-compat; safety) | design text | no copy; the PREV stepper does one PG per iteration |
| A22 | Raw hex for PGs without CLI names (resource-compat; compatibility) | `pg/servos.h:27-36`, `pg/mixer.h:116-132`, `pg/rx.h:69-72` | element formatters in CLI print order |
| A23 | Unknown headers rendered as raw HTML (compatibility) | `js/header_dialog.js:458-460` | escape (4.1); the firmware escapes `<>&` too |
| A24 | Events before the first I-frame dropped (all) | `js/flightlog_index.js:157-160` | written after frames; the viewer fix in 4.1 |
| A25 | Consumers outside the PID and filter tasks (exactness, all designs) | `rc.c:222`, `tasks.c:164` | transition frames (2.5, 4.3) |
| A26 | Side finding: rate limits indexed by profile, not by rates type | `rc_rates.c:430` vs `config.c:186-190` | c6 |

---

## Appendix B. Rejected alternatives

- **A 16-bit or hash shadow per setting** (the lead's draft). It is not exact, and the PG copies give a byte-exact shadow at 0 B.
- **The active profiles only in the header.** There are 283 in-flight switches, and `MSP_COPY_PROFILE` writes inactive profiles.
- **A snapshot streamed after T0** (PJ). It costs PID-task CPU and blocking SPI in flight, and it is lost when the log is cut early.
- **A deferred compare in the PID task** (minimal). It gives wrong stamps and costs PID-task time after each change.
- **A new event id.** Old decoders lose frames on an unknown event (compatibility probe `design/compat-upstream/len101.cjs`).
- **Binary `CUSTOM_DATA` (100).** The ESC sensor already uses it (`esc_sensor.c:4074`), and it does not describe itself.
- **A header of non-default values only.** The viewer would need the defaults of each exact build, target and custom-defaults set.
- **Pending and class tables in the firmware** (PJ). The firmware records facts (`C`, `A`, `R`, `boot.*`), and the viewer interprets them. The table can then be corrected without a reflash, and old logs can be read again.

---

## Appendix C. Decisions for the user

1. **Fork or upstream.** Build the fork with FULL for the two helicopters and offer PR-A to PR-D upstream with CHANGES as the default? Recommended: yes.
2. **Shadow.** Reuse `pg->copy` (0 B, guarded), or pay 2.7 KB for `BBP_OWN_SHADOW` (about 4.8 KB stays free on G474)? Recommended: `pg->copy`.
3. **PR-C in the fork.** Each fix changes when a value takes effect, and makes it equal to the other armed MSP sets. Recommended: c1, c3, c4, c5 and c6, plus c2 in its "reload" form. The journal is exact either way.
4. **Header budget.** Keep 64 B per iteration (stock), or use 128 B per iteration for FLASH after the bench test? The Fireball blind window is then about 245 ms instead of about 360 ms.
5. **Interim grace period.** Use 1 s (recommended) or 0 s (5.1).
