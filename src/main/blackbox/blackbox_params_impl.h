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
 * Parameter log: definitions shared by blackbox_params.c, blackbox_params_format.c and
 * blackbox_params_tables.c. Not an API for other modules.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cli/settings.h"
#include "pg/pg.h"

#define BBP_LOG_VERSION     1       // "H Param log:<n>"
#ifndef BBP_LINE_MAX
#define BBP_LINE_MAX        192     // longest header line, "H " and "\n" included
#endif
#define BBP_SCAN_BYTES      128     // parameter bytes compared per logged iteration by the journal scan

// Bytes in one "pg.<pgn>+<off>" line: 64, or less when "H boot.pg.4095+4095:" and the hex do not fit in a line
#define BBP_RAW_MAX         ((BBP_LINE_MAX - 21) / 2 < 64 ? (BBP_LINE_MAX - 21) / 2 : 64)

#define BBP_NONE            0xFFFF

/* Tracked parameter groups */

#define BBP_TRACKED         30

#define BBP_PG_BOOT         0x01    // its module caches it at boot: the header also gives the boot values

typedef struct {
    pgn_t       pgn;
    uint16_t    size;               // sizeof the group
    uint8_t     flags;              // BBP_PG_*
} bbpTrackedPg_t;

extern const bbpTrackedPg_t bbpTrackedPgs[BBP_TRACKED];

// The copy of the boot-cached groups at the end of init(), at bbpBootOffset(t)
extern uint8_t bbpBootCopy[];
uint16_t bbpBootOffset(int t);

#ifdef BLACKBOX_PARAMS_OWN_SHADOW
extern uint8_t bbpOwnShadow[];
uint16_t bbpShadowOffset(int t);
#endif

// Bytes of a boot-cached group that change at run time and are not boot values
bool bbpBootIgnored(pgn_t pgn, unsigned offset);

typedef struct {
    const pgRegistry_t *reg;        // NULL: the group is not in this build
    uint16_t    vtFirst;            // its valueTable entries are in vtFirst..vtEnd-1 (with entries of other groups
    uint16_t    vtEnd;              // between them if the table is not grouped)
} bbpPg_t;

extern bbpPg_t bbpPg[BBP_TRACKED];

// Bit t: the header gives every valueTable entry of group t as text (bbpValueInHeader)
extern uint32_t bbpPgAllInHeader;

// Index in bbpTrackedPgs of a registered tracked group, or -1
int bbpTrackedIndex(pgn_t pgn);

static inline uint8_t *bbpShadow(int t)
{
#ifdef BLACKBOX_PARAMS_OWN_SHADOW
    return bbpOwnShadow + bbpShadowOffset(t);
#else
    return bbpPg[t].reg->copy;
#endif
}

/* Elements: groups that the CLI sets with commands, not with "set" */

typedef enum {
    BBP_F_U8 = 0,
    BBP_F_S8,
    BBP_F_U16,
    BBP_F_S16,
    BBP_F_HEX32,
} bbpFieldType_e;

typedef struct {
    uint8_t     offset;
    uint8_t     type;               // bbpFieldType_e
} bbpField_t;

typedef struct {
    const char *name;               // "el.<name>.<i>"
    pgn_t       pgn;
    uint8_t     indexed;            // false: one element, "el.<name>"
    uint8_t     fieldCount;
    const bbpField_t *fields;
} bbpElementKind_t;

#define BBP_ELEMENT_KINDS   5

extern const bbpElementKind_t bbpElementKinds[BBP_ELEMENT_KINDS];

const bbpElementKind_t *bbpElementKind(pgn_t pgn);

/* Text */

// A bounded text writer. Nothing is written past 'end'; 'overflow' tells that the text did not fit.
typedef struct {
    char       *ptr;
    char       *end;
    bool        overflow;
} bbpWriter_t;

void bbpPutChar(bbpWriter_t *w, char c);
void bbpPutStr(bbpWriter_t *w, const char *s);
void bbpPutInt(bbpWriter_t *w, int32_t value);
void bbpPutUint(bbpWriter_t *w, uint32_t value);
void bbpPutHex(bbpWriter_t *w, uint32_t value);
void bbpPutHexBytes(bbpWriter_t *w, const uint8_t *p, int count);

// The text that CLI "get" prints (printValuePointer), with %XX escapes in strings
void bbpPutValue(bbpWriter_t *w, const clivalue_t *v, const void *p);
// The longest text that bbpPutValue() can write for this entry
int bbpValueMaxLength(const clivalue_t *v);
// Bytes of the value at v->offset that its text gives (bitsets: none)
int bbpValueWidth(const clivalue_t *v);

void bbpPutElement(bbpWriter_t *w, const bbpElementKind_t *kind, const uint8_t *p);

// A valueTable entry that the header gives as text. Other bytes are given as raw hex.
bool bbpValueInHeader(const clivalue_t *v);

// The same for an entry of group t, with the result of the boot-time check of the group
static inline bool bbpEntryInHeader(int t, const clivalue_t *v)
{
    return (bbpPgAllInHeader & (1U << t)) || bbpValueInHeader(v);
}

// Byte of group t that a set or element line gives
bool bbpByteCovered(int t, unsigned offset);

/* Header section */

void bbpHeaderBegin(bool full);
// The next header line in line[BBP_LINE_MAX]: its length, 0 when there is no line in this call, -1 when done
int bbpHeaderNextLine(char *line);
