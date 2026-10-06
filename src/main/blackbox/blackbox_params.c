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
#include <string.h>

#include "platform.h"

#ifdef USE_BLACKBOX

#include "common/maths.h"
#include "common/utils.h"

#include "cli/settings.h"

#include "pg/blackbox.h"
#include "pg/pg.h"

#include "blackbox/blackbox_io.h"
#include "blackbox/blackbox_params.h"
#include "blackbox/blackbox_params_impl.h"

bbpPg_t bbpPg[BBP_TRACKED];
uint32_t bbpPgAllInHeader;

static char bbpLine[BBP_LINE_MAX];

static struct {
    bool        active;             // the log has a parameter section
    bool        shadowValid;        // the shadow is a complete copy of the tracked groups
    uint8_t     lineLength;         // the header line in bbpLine
    uint8_t     linePos;            // its bytes that are written
    uint32_t    seq;                // journal records of this log
} bbp;

// bbpTrackedIndex(): the last group that it looked for. valueTable is grouped by parameter group.
static pgn_t cachedPgn = BBP_NONE;
static int8_t cachedIndex = -1;

int bbpTrackedIndex(pgn_t pgn)
{
    if (pgn != cachedPgn) {
        cachedPgn = pgn;
        cachedIndex = -1;
        for (int t = 0; t < BBP_TRACKED; t++) {
            if (bbpTrackedPgs[t].pgn == pgn) {
                if (bbpPg[t].reg) {
                    cachedIndex = t;
                }
                break;
            }
        }
    }
    return cachedIndex;
}

void blackboxParamsInit(void)
{
    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = pgFind(bbpTrackedPgs[t].pgn);
        // A size other than that of the table would overrun the copies
        bbpPg[t].reg = (reg && pgSize(reg) == bbpTrackedPgs[t].size) ? reg : NULL;
        bbpPg[t].vtFirst = 0;
        bbpPg[t].vtEnd = 0;
    }
    cachedPgn = BBP_NONE;

    bbpPgAllInHeader = 0;
    uint32_t tooLong = 0;
    for (unsigned i = 0; i < valueTableEntryCount; i++) {
        const int t = bbpTrackedIndex(valueTable[i].pgn);
        if (t >= 0) {
            if (bbpPg[t].vtEnd == 0) {
                bbpPg[t].vtFirst = i;
            }
            bbpPg[t].vtEnd = i + 1;
            if (!bbpValueInHeader(&valueTable[i])) {
                tooLong |= BIT(t);
            }
        }
    }
    bbpPgAllInHeader = ~tooLong;
}

void blackboxParamsBoot(void)
{
    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = bbpPg[t].reg;
        if (reg) {
            if (bbpTrackedPgs[t].flags & BBP_PG_BOOT) {
                memcpy(bbpBootCopy + bbpBootOffset(t), reg->address, pgSize(reg));
            }
            memcpy(bbpShadow(t), reg->address, pgSize(reg));
        }
    }
    bbp.shadowValid = true;
}

void blackboxParamsShadowLost(void)
{
#ifndef BLACKBOX_PARAMS_OWN_SHADOW
    bbp.shadowValid = false;
#endif
}

void blackboxParamsStart(void)
{
    const uint8_t mode = blackboxConfig()->params;

    bbp.active = (mode != BLACKBOX_PARAMS_OFF);
    bbp.seq = 0;
    bbp.lineLength = 0;
    bbp.linePos = 0;

    if (bbp.active) {
        bbpHeaderBegin(mode != BLACKBOX_PARAMS_CHANGES);
    }
}

bool blackboxParamsWriteHeader(void)
{
    if (!bbp.active) {
        return true;
    }

    if (bbp.linePos >= bbp.lineLength) {
        const int length = bbpHeaderNextLine(bbpLine);
        if (length < 0) {
            return true;
        }
        bbp.lineLength = length;
        bbp.linePos = 0;
    }

    // At most one line in each call, and at most 64 B: the rate of the other header states
    const int count = MIN(MIN(bbp.lineLength - bbp.linePos, blackboxHeaderBudget), BLACKBOX_TARGET_HEADER_BUDGET_PER_ITERATION);
    for (int i = 0; i < count; i++) {
        blackboxWrite(bbpLine[bbp.linePos + i]);
    }
    if (count > 0) {
        bbp.linePos += count;
        blackboxHeaderBudget -= count;
    }

    return false;
}

uint32_t blackboxParamsSeq(void)
{
    return bbp.seq;
}

#endif
