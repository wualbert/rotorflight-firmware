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

/*
 * Parameter log (blackbox_params): the parameter section of the log header.
 *
 * With blackbox_params = CHANGES or FULL, the header of each log gets a section that starts with
 * "H Param log:1" and ends with "H param_end:<lines>,<fnv>". FULL adds a snapshot of every byte of the
 * tracked parameter groups, as CLI text where the CLI names the bytes. OFF writes nothing.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#ifdef USE_BLACKBOX

// blackboxInit(): find the tracked parameter groups
void blackboxParamsInit(void);

// End of init(): keep the boot values of the groups that their modules cache at boot
void blackboxParamsBoot(void);

// blackboxStart(): latch blackbox_params for this log
void blackboxParamsStart(void);

// blackboxWriteSysinfo(): write the next part of the parameter section. True when it is complete.
bool blackboxParamsWriteHeader(void);

// S-frame paramSeq: the number of the last journal record of this log (0: none)
uint32_t blackboxParamsSeq(void);

// The CLI overwrote the PG copies (the shadow)
void blackboxParamsShadowLost(void);

#else

static inline void blackboxParamsBoot(void) { }
static inline void blackboxParamsShadowLost(void) { }

#endif
