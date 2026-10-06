/*
 * This file is part of Rotorflight.
 *
 * Rotorflight is free software. You can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rotorflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "platform.h"

#ifdef USE_BLACKBOX

#include "common/utils.h"

#include "pg/pg_ids.h"
#include "pg/accel.h"
#include "pg/arming.h"
#include "pg/battery.h"
#include "pg/blackbox.h"
#include "pg/boardalignment.h"
#include "pg/current.h"
#include "pg/dyn_notch.h"
#include "pg/esc_sensor.h"
#include "pg/failsafe.h"
#include "pg/feature.h"
#include "pg/freq.h"
#include "pg/governor.h"
#include "pg/gyro.h"
#include "pg/gyrodev.h"
#include "pg/imu.h"
#include "pg/mixer.h"
#include "pg/motor.h"
#include "pg/pid.h"
#include "pg/position.h"
#include "pg/rates.h"
#include "pg/rpm_filter.h"
#include "pg/rx.h"
#include "pg/servos.h"
#include "pg/system.h"
#include "pg/voltage.h"

#include "blackbox/blackbox_params_impl.h"

/*
 * The tracked parameter groups, in snapshot order: the flight-relevant configuration.
 *
 * Not tracked: STATS_CONFIG (it changes at each disarm), ADJUSTMENT_RANGE_CONFIG and MODE_ACTIVATION_PROFILE
 * (their effects are in the log as events and as S-frame flightModeFlags), SERVO_CONFIG (pin tags only), and
 * the LED, OSD, VTX, telemetry, serial, pin, timer, GPS, barometer and compass groups.
 *
 * BBP_PG_BOOT: a module reads the group only at boot, so the values in use can be the boot values.
 * The second column is the PG variable, for the size.
 */
#define BBP_TRACKED_PG_LIST(X) \
    X(PG_PID_PROFILE,                   pidProfiles_SystemArray,                0) \
    X(PG_CONTROL_RATE_PROFILES,         controlRateProfiles_SystemArray,        0) \
    X(PG_SYSTEM_CONFIG,                 systemConfig_System,                    BBP_PG_BOOT) \
    X(PG_PID_CONFIG,                    pidConfig_System,                       BBP_PG_BOOT) \
    X(PG_GYRO_CONFIG,                   gyroConfig_System,                      BBP_PG_BOOT) \
    X(PG_GYRO_DEVICE_CONFIG,            gyroDeviceConfig_SystemArray,           BBP_PG_BOOT) \
    X(PG_ACCELEROMETER_CONFIG,          accelerometerConfig_System,             BBP_PG_BOOT) \
    X(PG_BOARD_ALIGNMENT,               boardAlignment_System,                  BBP_PG_BOOT) \
    X(PG_IMU_CONFIG,                    imuConfig_System,                       0) \
    X(PG_DYN_NOTCH_CONFIG,              dynNotchConfig_System,                  BBP_PG_BOOT) \
    X(PG_RPM_FILTER_CONFIG,             rpmFilterConfig_System,                 BBP_PG_BOOT) \
    X(PG_GOVERNOR_CONFIG,               governorConfig_System,                  BBP_PG_BOOT) \
    X(PG_MOTOR_CONFIG,                  motorConfig_System,                     BBP_PG_BOOT) \
    X(PG_ESC_SENSOR_CONFIG,             escSensorConfig_System,                 BBP_PG_BOOT) \
    X(PG_FREQ_SENSOR_CONFIG,            freqConfig_System,                      BBP_PG_BOOT) \
    X(PG_GENERIC_MIXER_CONFIG,          mixerConfig_System,                     BBP_PG_BOOT) \
    X(PG_GENERIC_MIXER_INPUTS,          mixerInputs_SystemArray,                0) \
    X(PG_GENERIC_MIXER_RULES,           mixerRules_SystemArray,                 0) \
    X(PG_SERVO_PARAMS,                  servoParams_SystemArray,                BBP_PG_BOOT) \
    X(PG_RC_CONTROLS_CONFIG,            rcControlsConfig_System,                0) \
    X(PG_RX_CONFIG,                     rxConfig_System,                        BBP_PG_BOOT) \
    X(PG_RX_FAILSAFE_CHANNEL_CONFIG,    rxFailsafeChannelConfigs_SystemArray,   0) \
    X(PG_FAILSAFE_CONFIG,               failsafeConfig_System,                  0) \
    X(PG_ARMING_CONFIG,                 armingConfig_System,                    0) \
    X(PG_BATTERY_CONFIG,                batteryConfig_System,                   0) \
    X(PG_VOLTAGE_SENSOR_ADC_CONFIG,     voltageSensorADCConfig_SystemArray,     0) \
    X(PG_CURRENT_SENSOR_ADC_CONFIG,     currentSensorADCConfig_SystemArray,     0) \
    X(PG_FEATURE_CONFIG,                featureConfig_System,                   0) \
    X(PG_BLACKBOX_CONFIG,               blackboxConfig_System,                  0) \
    X(PG_POSITION,                      positionConfig_System,                  0)

// Copies start at 4-byte boundaries, for word compares
#define BBP_ALIGNED(size)                   (((size) + 3) & ~3)

#define BBP_TRACKED_ENTRY(pgn, var, flags)  { (pgn), sizeof(var), (flags) },
#define BBP_BOOT_SIZE(pgn, var, flags)      + (((flags) & BBP_PG_BOOT) ? BBP_ALIGNED(sizeof(var)) : 0)
#define BBP_TRACKED_SIZE(pgn, var, flags)   + BBP_ALIGNED(sizeof(var))

const bbpTrackedPg_t bbpTrackedPgs[] = {
    BBP_TRACKED_PG_LIST(BBP_TRACKED_ENTRY)
};

STATIC_ASSERT(ARRAYLEN(bbpTrackedPgs) == BBP_TRACKED, bbp_tracked_count);
STATIC_ASSERT(BBP_TRACKED <= 32, bbp_tracked_mask);     // bit masks of tracked groups

// Sized for every group of the list, also for a group that is not in this build
uint8_t bbpBootCopy[0 BBP_TRACKED_PG_LIST(BBP_BOOT_SIZE)] __attribute__((aligned(4)));

uint16_t bbpBootOffset(int t)
{
    uint16_t offset = 0;

    for (int i = 0; i < t; i++) {
        if (bbpTrackedPgs[i].flags & BBP_PG_BOOT) {
            offset += BBP_ALIGNED(bbpTrackedPgs[i].size);
        }
    }
    return offset;
}

#ifdef BLACKBOX_PARAMS_OWN_SHADOW
uint8_t bbpOwnShadow[0 BBP_TRACKED_PG_LIST(BBP_TRACKED_SIZE)] __attribute__((aligned(4)));

uint16_t bbpShadowOffset(int t)
{
    uint16_t offset = 0;

    for (int i = 0; i < t; i++) {
        offset += BBP_ALIGNED(bbpTrackedPgs[i].size);
    }
    return offset;
}
#endif

bool bbpBootIgnored(pgn_t pgn, unsigned offset)
{
    // The profile indices change with each profile switch: the S-frames give them
    return pgn == PG_SYSTEM_CONFIG &&
        (offset == offsetof(systemConfig_t, pidProfileIndex) || offset == offsetof(systemConfig_t, activeRateProfile));
}

/*
 * Elements, in the order of the CLI dump. Each field is a decimal number, except HEX32.
 */
static const bbpField_t servoFields[] = {
    { offsetof(servoParam_t, mid),      BBP_F_U16 },
    { offsetof(servoParam_t, min),      BBP_F_S16 },
    { offsetof(servoParam_t, max),      BBP_F_S16 },
    { offsetof(servoParam_t, rneg),     BBP_F_U16 },
    { offsetof(servoParam_t, rpos),     BBP_F_U16 },
    { offsetof(servoParam_t, rate),     BBP_F_U16 },
    { offsetof(servoParam_t, speed),    BBP_F_U16 },
    { offsetof(servoParam_t, flags),    BBP_F_U16 },
};

static const bbpField_t mixerInputFields[] = {
    { offsetof(mixerInput_t, rate),     BBP_F_S16 },
    { offsetof(mixerInput_t, min),      BBP_F_S16 },
    { offsetof(mixerInput_t, max),      BBP_F_S16 },
};

static const bbpField_t mixerRuleFields[] = {
    { offsetof(mixerRule_t, oper),      BBP_F_U8 },
    { offsetof(mixerRule_t, input),     BBP_F_U8 },
    { offsetof(mixerRule_t, output),    BBP_F_U8 },
    { offsetof(mixerRule_t, offset),    BBP_F_S16 },
    { offsetof(mixerRule_t, weight),    BBP_F_S16 },
};

static const bbpField_t rxFailsafeFields[] = {
    { offsetof(rxFailsafeChannelConfig_t, mode),    BBP_F_U8 },
    { offsetof(rxFailsafeChannelConfig_t, step),    BBP_F_U8 },
};

static const bbpField_t featureFields[] = {
    { offsetof(featureConfig_t, enabledFeatures),   BBP_F_HEX32 },
};

const bbpElementKind_t bbpElementKinds[BBP_ELEMENT_KINDS] = {
    { "servo",      PG_SERVO_PARAMS,                true,   ARRAYLEN(servoFields),      servoFields },
    { "mixin",      PG_GENERIC_MIXER_INPUTS,        true,   ARRAYLEN(mixerInputFields), mixerInputFields },
    { "mixrule",    PG_GENERIC_MIXER_RULES,         true,   ARRAYLEN(mixerRuleFields),  mixerRuleFields },
    { "rxfail",     PG_RX_FAILSAFE_CHANNEL_CONFIG,  true,   ARRAYLEN(rxFailsafeFields), rxFailsafeFields },
    { "feature",    PG_FEATURE_CONFIG,              false,  ARRAYLEN(featureFields),    featureFields },
};

const bbpElementKind_t *bbpElementKind(pgn_t pgn)
{
    for (int i = 0; i < BBP_ELEMENT_KINDS; i++) {
        if (bbpElementKinds[i].pgn == pgn) {
            return &bbpElementKinds[i];
        }
    }
    return NULL;
}

#endif
