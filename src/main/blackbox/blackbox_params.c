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
 * Parameter log: state, the shadow, the journal ring, the capture, operations, the drain, the scan and the
 * runtime polls.
 *
 * Journal events (CUSTOM_STRING, at most 128 chars):
 *
 *   P<type><seq> <at> <fields>*<crc>
 *
 *   type    C change, A apply, R runtime, M marker, L lost, Q end, + continuation of the record <seq>
 *   seq     record number in this log, lowercase hex, from 1. The S-frame field paramSeq is the last one.
 *   at      "p" (before T0), <iteration>.<tick>, or <iteration>.<tick>~<iteration>.<tick> (an interval)
 *   crc     CRC-8 (polynomial 0xD5, initial value 0) of the chars before '*', two uppercase hex digits
 *
 *   C   s=<src>[.<arg>] [pgs=<pgn>] n=<items> <key>=<new><<old> ...   ('+' events continue the items)
 *   A   <loader>[/<slot>] [fp=<hex>]
 *   R   gov_mode=<n> features=<hex> fp.pid=<hex> fp.gov=<hex> fp.sp=<hex>   (any subset)
 *   M   eesave us=<n> | eeload | escparam n=<len> fnv=<hex> | shadow-reset
 *   L   pgs=<pgn>,...                                                     ('+' events continue the list)
 *   Q   unsent=<n> lostrec=<n>
 *
 * Records of source u (no hook saw the write), v (a change between logs) and y (a resync after a ring
 * overflow) have their own record for each group, with an interval from the last point where the group was
 * proved equal to the shadow. A y record gives "pgs=<pgn>", also when it has no items.
 *
 * The task that changes the configuration captures the change: a word compare of the groups with the shadow
 * (the PG copies) and a walk of the changed words. The records wait in a RAM ring. The PID task writes at most
 * one event after each logged frame, and only when 192 B stay free in the device buffer for frames.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#ifdef USE_BLACKBOX

#include "common/maths.h"
#include "common/utils.h"

#include "cli/settings.h"

#include "config/feature.h"

#include "drivers/time.h"

#include "fc/core.h"

#include "flight/governor.h"
#include "flight/pid.h"
#include "flight/setpoint.h"

#include "msp/msp_protocol.h"
#include "msp/msp_protocol_v2_betaflight.h"

#include "pg/blackbox.h"
#include "pg/pg.h"
#include "pg/pg_ids.h"
#include "pg/system.h"

#include "blackbox/blackbox.h"
#include "blackbox/blackbox_io.h"
#include "blackbox/blackbox_params.h"
#include "blackbox/blackbox_params_impl.h"

#define BBP_RING_SIZE       1024    // binary records, a power of 2
#define BBP_RING_KEEP       32      // ring bytes kept free for an 'L' record
#define BBP_REC_HEADER      22
#define BBP_FRAME_RESERVE   192     // device bytes left free for frames after an event (about 4 frames)
#define BBP_HOLD_MS         100     // longest delay of LOG_END while records wait
#define BBP_RESYNC_FREE     256     // ring bytes free before a resync after an overflow
#define BBP_DIFF_WORDS      8       // changed-word bitmap: a group of at most 1024 B

STATIC_ASSERT((BBP_RING_SIZE & (BBP_RING_SIZE - 1)) == 0, bbp_ring_size_power_of_2);

// Runtime values that the journal polls
enum {
    BBP_RT_GOV_MODE = 0,
    BBP_RT_FEATURES,
    BBP_RT_FP_PID,
    BBP_RT_FP_GOV,
    BBP_RT_FP_SP,
    BBP_RT_COUNT
};

static const char * const runtimeNames[BBP_RT_COUNT] = {
    "gov_mode=", "features=", "fp.pid=", "fp.gov=", "fp.sp="
};

static const char * const markerNames[BBP_MARKER_COUNT] = {
    "eesave", "eeload", "escparam", "shadow-reset"
};

static const bbpPoint_t pointPre = { BBP_PRE, 0 };

bbpPg_t bbpPg[BBP_TRACKED];
uint32_t bbpPgAllInHeader;

// One header line or one journal event
static char bbpLine[(BBP_LINE_MAX > BBP_EVENT_MAX) ? BBP_LINE_MAX : BBP_EVENT_MAX + 1];

static uint8_t bbpRing[BBP_RING_SIZE];

// The last point at which group t was proved equal to the shadow, whole
static bbpPoint_t bbpVerified[BBP_TRACKED];

static struct {
    bool        active;             // the log has a parameter section and a journal
    bool        shadowValid;        // the shadow is a complete copy of the tracked groups
    bool        t0;                 // the log is RUNNING (T0 passed)

    uint32_t    allMask;            // the tracked groups of this build
    uint32_t    wordMask;           // groups that the capture compares in words (aligned live RAM and shadow)
    uint32_t    profileOnlyMask;    // groups whose valueTable entries are all profile values
    int8_t      systemIndex;        // SYSTEM_CONFIG in bbpTrackedPgs

    // Header
    uint8_t     lineLength;         // the header line in bbpLine
    uint8_t     linePos;            // its bytes that are written

    // Operations
    uint8_t     opDepth;
    char        opSrc;
    uint16_t    opArg;

    // Records
    uint32_t    seq;                // records created in this log: S-frame paramSeq
    uint32_t    prevMask;           // groups not yet compared with the previous log
    uint32_t    lostMask;           // groups with differences that the ring did not take
    uint32_t    lostListed;         // ... that an 'L' record lists
    bbpPoint_t  lostAt;
    uint16_t    lostRec;            // records with a seq that are not in the ring (A, R, M)
    uint16_t    ringHead;           // free running
    uint16_t    ringTail;

    // Drain
    bool        eventReady;         // an event is in bbpLine
    bool        drainStarted;       // the first event of the record at ringTail is written
    bool        drainDone;          // the event in bbpLine is the last of its record
    uint16_t    drainPos;           // next item of that record (L: next group)
    uint16_t    drainNext;
    uint8_t     eventLength;

    // Scan
    uint8_t     scanPg;
    uint16_t    scanOff;
    bbpPoint_t  scanPassStart;

    // Runtime polls
    uint8_t     rtIndex;
    uint8_t     rtTick;
    uint32_t    rtLast[BBP_RT_COUNT];
    bbpPoint_t  rtPolled[BBP_RT_COUNT];

    // LOG_END hold
    bool        holding;
    uint32_t    holdIteration;
    timeMs_t    holdStart;
} bbp;

// The record that a capture builds
static struct {
    bool        open;
    bool        partial;            // items did not fit
    uint8_t     type;
    char        src;
    uint16_t    arg;
    uint32_t    seq;
    bbpPoint_t  from;
    bbpPoint_t  at;
    uint16_t    start;
    uint16_t    pos;
    uint16_t    count;
} rec;

/* Tracked groups */

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

static bool aligned(const void *p)
{
    return ((uintptr_t)p & 3) == 0;
}

void blackboxParamsInit(void)
{
    bbp.allMask = 0;
    bbp.wordMask = 0;
    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = pgFind(bbpTrackedPgs[t].pgn);
        // A size other than that of the table would overrun the copies
        bbpPg[t].reg = (reg && pgSize(reg) == bbpTrackedPgs[t].size && pgSize(reg) <= BBP_DIFF_WORDS * 128) ? reg : NULL;
        bbpPg[t].vtFirst = 0;
        bbpPg[t].vtEnd = 0;
        if (bbpPg[t].reg) {
            bbp.allMask |= BIT(t);
            if (aligned(reg->address) && aligned(bbpShadow(t))) {
                bbp.wordMask |= BIT(t);
            }
        }
    }
    cachedPgn = BBP_NONE;
    bbp.systemIndex = bbpTrackedIndex(PG_SYSTEM_CONFIG);

    bbpPgAllInHeader = 0;
    uint32_t tooLong = 0;
    uint32_t master = 0;
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
            const int section = valueTable[i].type & VALUE_SECTION_MASK;
            if (section != PROFILE_VALUE && section != PROFILE_RATE_VALUE) {
                master |= BIT(t);
            }
        }
    }
    bbpPgAllInHeader = ~tooLong;
    bbp.profileOnlyMask = ~master;

    bbpTablesInit();
}

/* Word loops: newlib-nano memcpy and memcmp work one byte per loop */

static void copyToShadow(int t)
{
    const pgRegistry_t *reg = bbpPg[t].reg;
    const unsigned size = pgSize(reg);
    unsigned i = 0;

    if (bbp.wordMask & BIT(t)) {
        const uint32_t *src = (const uint32_t *)reg->address;
        uint32_t *dst = (uint32_t *)bbpShadow(t);
        for (; i < size / 4; i++) {
            dst[i] = src[i];
        }
        i *= 4;
    }
    for (; i < size; i++) {
        bbpShadow(t)[i] = reg->address[i];
    }
}

// The first block of 8 words from word w with a difference, or the first block that does not fit before wEnd.
// The hot loop of the capture: about 1.2 cycles per byte on a Cortex-M4.
static unsigned firstChangedBlock(const uint32_t *a, const uint32_t *b, unsigned w, unsigned wEnd)
{
    const uint32_t *p = a + w;
    const uint32_t *q = b + w;

    for (; w + 8 <= wEnd; w += 8, p += 8, q += 8) {
        const uint32_t d = (p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3]) |
            (p[4] ^ q[4]) | (p[5] ^ q[5]) | (p[6] ^ q[6]) | (p[7] ^ q[7]);
        if (d) {
            break;
        }
    }
    return w;
}

// Bytes start..end-1 of group t differ from the shadow. start is a multiple of 4. The test of most groups in
// most captures: no bitmap.
static bool groupDiffers(int t, unsigned start, unsigned end)
{
    const uint8_t *live = bbpPg[t].reg->address;
    const uint8_t *shadow = bbpShadow(t);
    unsigned i = start;

    if (bbp.wordMask & BIT(t)) {
        const uint32_t *a = (const uint32_t *)live;
        const uint32_t *b = (const uint32_t *)shadow;
        const unsigned wEnd = end / 4;
        unsigned w = firstChangedBlock(a, b, start / 4, wEnd);
        if (w + 8 <= wEnd) {
            return true;
        }
        for (; w < wEnd; w++) {
            if (a[w] != b[w]) {
                return true;
            }
        }
        i = wEnd * 4;
    }
    for (; i < end; i++) {
        if (live[i] != shadow[i]) {
            return true;
        }
    }
    return false;
}

// Compare bytes start..end-1 of group t with the shadow. Bit w of map: bytes 4w..4w+3 differ.
static bool diffGroup(int t, unsigned start, unsigned end, uint32_t *map)
{
    const uint8_t *live = bbpPg[t].reg->address;
    const uint8_t *shadow = bbpShadow(t);
    uint32_t any = 0;

    for (int i = 0; i < BBP_DIFF_WORDS; i++) {
        map[i] = 0;
    }

    unsigned w = start / 4;
    if (bbp.wordMask & BIT(t)) {
        const uint32_t *a = (const uint32_t *)live;
        const uint32_t *b = (const uint32_t *)shadow;
        const unsigned wEnd = end / 4;
        while (w + 8 <= wEnd) {
            w = firstChangedBlock(a, b, w, wEnd);
            if (w + 8 > wEnd) {
                break;
            }
            for (unsigned k = w; k < w + 8; k++) {
                if (a[k] != b[k]) {
                    map[k / 32] |= BIT(k % 32);
                }
            }
            any = 1;
            w += 8;
        }
        for (; w < wEnd; w++) {
            if (a[w] != b[w]) {
                map[w / 32] |= BIT(w % 32);
                any = 1;
            }
        }
    }
    for (unsigned i = w * 4; i < end; i++) {
        if (live[i] != shadow[i]) {
            map[i / 128] |= BIT((i / 4) % 32);
            any = 1;
        }
    }
    return any;
}

// A changed word covers some of the bytes offset..offset+width-1
static bool mapHas(const uint32_t *map, unsigned offset, unsigned width)
{
    for (unsigned w = offset / 4; w <= (offset + width - 1) / 4; w++) {
        if (map[w / 32] & BIT(w % 32)) {
            return true;
        }
    }
    return false;
}

static bool bytesDiffer(const uint8_t *a, const uint8_t *b, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return true;
        }
    }
    return false;
}

static void copyBytes(uint8_t *dst, const uint8_t *src, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

// Little endian, 1-4 bytes
static uint32_t readBytes(const uint8_t *p, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = count; i > 0; i--) {
        value = (value << 8) | p[i - 1];
    }
    return value;
}

static void writeBytes(uint8_t *p, unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        p[i] = value >> (8 * i);
    }
}

/* Points */

static bbpPoint_t pointNow(void)
{
    if (!blackboxIsLogRunning()) {
        return pointPre;
    }
    // Ticks 0..c-1 of this PID cycle have run. With c > b, blackboxUpdate of this cycle already advanced the
    // iteration: the ticks that are left (flush, and the filter update with pid_process_denom 3, which acts on
    // the next cycle) do not change the frame, which is new in all fields.
    const uint8_t c = getPidUpdateCounter();
    const bbpPoint_t point = { blackboxGetIteration(), (c <= coreSubtaskTick(CORE_ST_BLACKBOX)) ? c : 0 };
    return point;
}

// In the PID task, after the frame of this iteration: the next frame is new in all fields
static bbpPoint_t pointAfterFrame(void)
{
    const bbpPoint_t point = { blackboxGetIteration() + 1, 0 };
    return point;
}

static bool pointAfter(bbpPoint_t a, bbpPoint_t b)
{
    if (a.n == BBP_PRE) {
        return false;
    }
    if (b.n == BBP_PRE) {
        return true;
    }
    return a.n > b.n || (a.n == b.n && a.c > b.c);
}

// The start of an interval at the last verified point of group t. Before T0 there are no frames: 0.0.
static bbpPoint_t intervalStart(int t, bbpPoint_t at)
{
    if (at.n == BBP_PRE) {
        return at;
    }
    if (bbpVerified[t].n == BBP_PRE) {
        const bbpPoint_t first = { 0, 0 };
        return first;
    }
    return bbpVerified[t];
}

/* Ring */

static uint16_t ringUsed(void)
{
    return (uint16_t)(bbp.ringHead - bbp.ringTail);
}

static unsigned ringFree(void)
{
    return BBP_RING_SIZE - ringUsed();
}

static uint8_t ringGet(uint16_t pos)
{
    return bbpRing[pos & (BBP_RING_SIZE - 1)];
}

static void ringPut(uint16_t pos, uint8_t value)
{
    bbpRing[pos & (BBP_RING_SIZE - 1)] = value;
}

static uint32_t ringGetN(uint16_t pos, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = count; i > 0; i--) {
        value = (value << 8) | ringGet(pos + i - 1);
    }
    return value;
}

static void ringPutN(uint16_t pos, unsigned count, uint32_t value)
{
    for (unsigned i = 0; i < count; i++) {
        ringPut(pos + i, value >> (8 * i));
    }
}

// Record header
#define REC_TYPE    0
#define REC_SRC     1
#define REC_ARG     2
#define REC_SEQ     4
#define REC_N0      8
#define REC_C0      12
#define REC_N1      13
#define REC_C1      17
#define REC_COUNT   18
#define REC_LEN     20

static bool recBegin(uint8_t type, char src, uint16_t arg, bbpPoint_t from, bbpPoint_t at)
{
    const unsigned keep = (type == 'L') ? 0 : BBP_RING_KEEP;
    if (ringFree() < BBP_REC_HEADER + keep) {
        if (type != 'C') {
            // The seq is used: the reader finds the gap
            bbp.seq++;
            bbp.lostRec++;
        }
        // A change that does not fit stays in the lost groups: 'L', then 'y' records
        return false;
    }
    rec.seq = ++bbp.seq;

    rec.open = true;
    rec.partial = false;
    rec.type = type;
    rec.src = src;
    rec.arg = arg;
    rec.from = from;
    rec.at = at;
    rec.start = bbp.ringHead;
    rec.pos = bbp.ringHead + BBP_REC_HEADER;
    rec.count = 0;
    return true;
}

static bool recSpace(unsigned bytes)
{
    const unsigned keep = (rec.type == 'L') ? 0 : BBP_RING_KEEP;
    return BBP_RING_SIZE - (unsigned)(uint16_t)(rec.pos - bbp.ringTail) >= bytes + keep;
}

static void recPut(const uint8_t *p, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        ringPut(rec.pos++, p[i]);
    }
}

static void recPutN(unsigned count, uint32_t value)
{
    ringPutN(rec.pos, count, value);
    rec.pos += count;
}

// Item: code (2), slot, index, length, new bytes, old bytes
static bool recItem(uint16_t code, uint8_t slot, uint8_t index, const uint8_t *newBytes, const uint8_t *oldBytes, uint8_t length)
{
    if (!recSpace(5 + 2 * length)) {
        rec.partial = true;
        return false;
    }
    recPutN(2, code);
    recPutN(1, slot);
    recPutN(1, index);
    recPutN(1, length);
    recPut(newBytes, length);
    recPut(oldBytes, length);
    rec.count++;
    return true;
}

static void recCommit(void)
{
    if (!rec.open) {
        return;
    }
    rec.open = false;

    if (rec.type == 'C' && rec.count == 0 && rec.src != 'y') {
        // Nothing to record. The items that did not fit are in the lost groups.
        if (rec.seq == bbp.seq) {
            bbp.seq--;
        }
        return;
    }

    const uint16_t h = rec.start;
    ringPutN(h + REC_TYPE, 1, rec.type);
    ringPutN(h + REC_SRC, 1, rec.src);
    ringPutN(h + REC_ARG, 2, rec.arg);
    ringPutN(h + REC_SEQ, 4, rec.seq);
    ringPutN(h + REC_N0, 4, rec.from.n);
    ringPutN(h + REC_C0, 1, rec.from.c);
    ringPutN(h + REC_N1, 4, rec.at.n);
    ringPutN(h + REC_C1, 1, rec.at.c);
    ringPutN(h + REC_COUNT, 2, rec.count);
    ringPutN(h + REC_LEN, 2, (uint16_t)(rec.pos - h - BBP_REC_HEADER));
    bbp.ringHead = rec.pos;
}

// A record with a payload of 32-bit values
static void recValues(uint8_t type, char src, uint16_t arg, bbpPoint_t from, bbpPoint_t at, const uint32_t *values, int count)
{
    if (!recBegin(type, src, arg, from, at)) {
        return;
    }
    if (!recSpace(4 * count)) {
        // The seq is used: the reader finds the gap
        rec.open = false;
        bbp.lostRec++;
        return;
    }
    for (int i = 0; i < count; i++) {
        recPutN(4, values[i]);
    }
    recCommit();
}

/* Capture */

// The bytes of an item from live RAM and the shadow. The shadow is synced.
static bool itemFromLive(int t, uint16_t code, uint8_t slot, uint8_t index, unsigned offset, unsigned length)
{
    const uint8_t *live = bbpPg[t].reg->address + offset;
    uint8_t *shadow = bbpShadow(t) + offset;

    if (!recItem(code, slot, index, live, shadow, length)) {
        return false;
    }
    copyBytes(shadow, live, length);
    return true;
}

// A string whose bytes after the first NUL are NUL: its text gives every byte
static bool stringIsText(const uint8_t *p, unsigned length)
{
    unsigned i = 0;
    while (i < length && p[i]) {
        i++;
    }
    while (i < length && !p[i]) {
        i++;
    }
    return i == length;
}

// Values that the CLI names: one item for each changed value, or each changed element of an array
static bool walkNamed(int t, const uint32_t *map, unsigned start, unsigned end)
{
    const pgRegistry_t *reg = bbpPg[t].reg;
    const pgn_t pgn = pgN(reg);
    const unsigned elementSize = pgElementSize(reg);
    const uint8_t *live = reg->address;
    uint8_t *shadow = bbpShadow(t);

    // Instance k: the profile values of profile k. Instance 0 also has the other values (offsets in the whole
    // group). A profile without a changed word costs one bitmap test.
    for (unsigned k = 0; k < reg->length; k++) {
        const unsigned base = k * elementSize;
        const bool profileChanged = base < end && base + elementSize > start && mapHas(map, base, elementSize);
        if (!profileChanged && (k > 0 || (bbp.profileOnlyMask & BIT(t)))) {
            continue;
        }
        for (unsigned i = bbpPg[t].vtFirst; i < bbpPg[t].vtEnd; i++) {
            const clivalue_t *v = &valueTable[i];
            if (v->pgn != pgn) {
                continue;
            }
            const int section = v->type & VALUE_SECTION_MASK;
            const bool perProfile = (section == PROFILE_VALUE || section == PROFILE_RATE_VALUE);
            if (perProfile ? !profileChanged : k > 0) {
                continue;
            }
            const int mode = v->type & VALUE_MODE_MASK;
            const unsigned size = bbpItemWidth(v);
            const unsigned width = (mode == MODE_ARRAY) ? v->config.array.length * size : size;
            const unsigned offset = (perProfile ? base : 0) + v->offset;
            if (offset < start || offset + width > end || !mapHas(map, offset, width)) {
                continue;
            }
            if (!bbpItemFits(v) || (mode == MODE_STRING && !(stringIsText(live + offset, width) && stringIsText(shadow + offset, width)))) {
                continue;   // raw items
            }
            const uint8_t slot = perProfile ? k : BBP_SLOT_NONE;
            if (mode == MODE_BITSET) {
                const uint32_t bit = BIT(v->config.bitpos);
                const uint32_t liveWord = readBytes(live + offset, size);
                const uint32_t shadowWord = readBytes(shadow + offset, size);
                if ((liveWord ^ shadowWord) & bit) {
                    if (!recItem(i, slot, BBP_SLOT_NONE, live + offset, shadow + offset, size)) {
                        return false;
                    }
                    writeBytes(shadow + offset, size, (shadowWord & ~bit) | (liveWord & bit));
                }
            } else if (mode == MODE_ARRAY) {
                for (unsigned e = 0; e < v->config.array.length; e++) {
                    const unsigned at = offset + e * size;
                    if (bytesDiffer(live + at, shadow + at, size) && !itemFromLive(t, i, slot, e, at, size)) {
                        return false;
                    }
                }
            } else if (bytesDiffer(live + offset, shadow + offset, width) && !itemFromLive(t, i, slot, BBP_SLOT_NONE, offset, width)) {
                return false;
            }
        }
    }
    return true;
}

static bool elementItemFits(uint16_t code, const uint8_t *newBytes, const uint8_t *oldBytes, unsigned size)
{
    char text[BBP_ITEM_MAX];
    bbpWriter_t w = { text, text + sizeof(text), false };

    return size <= BBP_ITEM_BYTES_MAX && bbpPutItem(&w, code, BBP_SLOT_NONE, BBP_SLOT_NONE, newBytes, oldBytes, size);
}

// Elements (servo, mixer input and rule, rx failsafe, features) and the profile indices of SYSTEM_CONFIG
static bool walkElements(int t, const uint32_t *map, unsigned start, unsigned end)
{
    const pgRegistry_t *reg = bbpPg[t].reg;
    const uint8_t *live = reg->address;
    uint8_t *shadow = bbpShadow(t);
    const bbpElementKind_t *kind = bbpElementKind(pgN(reg));

    if (kind) {
        const unsigned size = pgElementSize(reg);
        const unsigned count = kind->indexed ? reg->length : 1;
        for (unsigned i = 0; i < count; i++) {
            const unsigned offset = i * size;
            if (offset < start || offset + size > end || !mapHas(map, offset, size)) {
                continue;
            }
            bool differs = false;
            for (int f = 0; f < kind->fieldCount; f++) {
                const unsigned at = offset + kind->fields[f].offset;
                differs = differs || bytesDiffer(live + at, shadow + at, bbpFieldWidth(&kind->fields[f]));
            }
            const uint16_t code = BBP_CODE_ELEMENT | (unsigned)(kind - bbpElementKinds) << 8 | i;
            if (!differs || !elementItemFits(code, live + offset, shadow + offset, size)) {
                continue;   // too long: raw items
            }
            if (!recItem(code, BBP_SLOT_NONE, BBP_SLOT_NONE, live + offset, shadow + offset, size)) {
                return false;
            }
            // The fields only: other bytes of the element are raw items
            for (int f = 0; f < kind->fieldCount; f++) {
                const unsigned at = offset + kind->fields[f].offset;
                copyBytes(shadow + at, live + at, bbpFieldWidth(&kind->fields[f]));
            }
        }
    }

    if (t == bbp.systemIndex) {
        static const struct {
            uint16_t code;
            uint8_t offset;
        } indices[] = {
            { BBP_CODE_PID_INDEX,   offsetof(systemConfig_t, pidProfileIndex) },
            { BBP_CODE_RATE_INDEX,  offsetof(systemConfig_t, activeRateProfile) },
        };
        for (unsigned i = 0; i < ARRAYLEN(indices); i++) {
            const unsigned at = indices[i].offset;
            if (at >= start && at < end && live[at] != shadow[at] && !itemFromLive(t, indices[i].code, BBP_SLOT_NONE, BBP_SLOT_NONE, at, 1)) {
                return false;
            }
        }
    }
    return true;
}

// Bytes that still differ: items of at most 16 bytes
static bool walkRaw(int t, const uint32_t *map, unsigned start, unsigned end)
{
    const uint8_t *live = bbpPg[t].reg->address;
    const uint8_t *shadow = bbpShadow(t);

    for (unsigned i = start; i < end; ) {
        const uint32_t bits = map[i / 128] >> ((i / 4) % 32);
        if (!bits) {
            i = (i | 127) + 1;      // no changed word in the rest of this bitmap word
            continue;
        }
        if (!(bits & 1)) {
            i = (i | 3) + 1;
            continue;
        }
        if (live[i] == shadow[i]) {
            i++;
            continue;
        }
        unsigned last = i;
        for (unsigned j = i + 1; j < end && j < i + BBP_RAW_ITEM_BYTES; j++) {
            if (live[j] != shadow[j]) {
                last = j;
            }
        }
        if (!itemFromLive(t, BBP_CODE_RAW | t, i & 0xFF, i >> 8, i, last - i + 1)) {
            return false;
        }
        i = last + 1;
    }
    return true;
}

/*
 * Compare the groups in 'set' with the shadow. In group slotGroup only profile 'slot' is compared, when slot >= 0.
 * Each difference becomes an item with the new and the old bytes, and the shadow is synced item by item.
 * attributed: the differences are of the operation, source src. Otherwise, and for groups with differences
 * from before (lost or between logs), each group with a difference gets its own record with an interval from
 * its last verified point.
 */
static void capture(bool attributed, char src, uint16_t arg, bbpPoint_t at, uint32_t set, int slot, int slotGroup)
{
    uint32_t map[BBP_DIFF_WORDS];
    int t;

    for (t = 0; t < BBP_TRACKED; t++) {
        if (!(set & BIT(t))) {
            continue;
        }
        const pgRegistry_t *reg = bbpPg[t].reg;
        const bool whole = (slot < 0 || t != slotGroup);
        const unsigned start = whole ? 0 : slot * pgElementSize(reg);
        const unsigned end = whole ? pgSize(reg) : start + pgElementSize(reg);

        if (!groupDiffers(t, start & ~3, end) || !diffGroup(t, start & ~3, end, map)) {
            if (whole) {
                bbpVerified[t] = at;
                bbp.prevMask &= ~BIT(t);
                if (bbp.lostMask & BIT(t)) {
                    // The lost bytes are back at their recorded values: a 'y' record without items
                    if (rec.open) {
                        recCommit();
                    }
                    if (recBegin('C', 'y', pgN(reg), intervalStart(t, at), at)) {
                        recCommit();
                        bbp.lostMask &= ~BIT(t);
                        bbp.lostListed &= ~BIT(t);
                    }
                }
            }
            continue;
        }

        const bool own = !attributed || ((bbp.lostMask | bbp.prevMask) & BIT(t));
        if (own) {
            if (rec.open) {
                recCommit();
            }
            const char s = (bbp.lostMask & BIT(t)) ? 'y' : (bbp.prevMask & BIT(t)) ? 'v' : 'u';
            if (!recBegin('C', s, (s == 'y') ? pgN(reg) : BBP_ARG_NONE, intervalStart(t, at), at)) {
                goto lost;
            }
        } else if (!rec.open && !recBegin('C', src, arg, at, at)) {
            goto lost;
        }

        if (!walkNamed(t, map, start, end) || !walkElements(t, map, start, end) || !walkRaw(t, map, start, end)) {
            goto lost;
        }

        if (whole) {
            bbpVerified[t] = at;
            bbp.prevMask &= ~BIT(t);
            bbp.lostMask &= ~BIT(t);
            bbp.lostListed &= ~BIT(t);
        }
        if (own) {
            recCommit();
        }
    }
    recCommit();
    return;

lost:
    // Items that fit stay valid. The shadow of the other differences stays unsynced: the PID task writes 'L',
    // then a 'y' record for each group.
    recCommit();
    if (!(bbp.lostMask & ~bbp.lostListed)) {
        bbp.lostAt = at;
    }
    bbp.lostMask |= BIT(t);
    for (t++; t < BBP_TRACKED; t++) {
        if (set & BIT(t)) {
            const bool whole = (slot < 0 || t != slotGroup);
            const unsigned start = whole ? 0 : slot * pgElementSize(bbpPg[t].reg);
            const unsigned end = whole ? pgSize(bbpPg[t].reg) : start + pgElementSize(bbpPg[t].reg);
            if (groupDiffers(t, start & ~3, end)) {
                bbp.lostMask |= BIT(t);
            }
        }
    }
    bbp.prevMask &= ~bbp.lostMask;
}

/* Operations */

void blackboxParamsOpBegin(char source, uint16_t arg)
{
    if (!bbp.active || bbp.opDepth++ > 0) {
        return;
    }
    bbp.opSrc = source;
    bbp.opArg = arg;
    // Differences from before the operation are not of the operation: u, v or y records with an interval
    capture(false, 'u', BBP_ARG_NONE, pointNow(), bbp.allMask, -1, -1);
}

void blackboxParamsOpEnd(void)
{
    if (!bbp.active || bbp.opDepth == 0 || --bbp.opDepth > 0) {
        return;
    }
    capture(true, bbp.opSrc, bbp.opArg, pointNow(), bbp.allMask, -1, -1);
}

bool blackboxParamsMspBegin(int16_t cmd)
{
    switch (cmd) {
    // No configuration change, or one operation for each command (MSP_MULTIPLE_MSP)
    case MSP_SET_RAW_RC:
    case MSP_DATAFLASH_READ:
    case MSP_SET_PASSTHROUGH:
    case MSP_SET_RTC:
    case MSP_SET_TX_INFO:
    case MSP_SET_RAW_GPS:
    case MSP_SET_HEADING:
    case MSP_SET_MOTOR:
    case MSP_SET_MOTOR_OVERRIDE:
    case MSP_SET_SERVO_OVERRIDE:
    case MSP_SET_SERVO_OVERRIDE_ALL:
    case MSP_SET_MIXER_OVERRIDE:
    case MSP2_SEND_DSHOT_COMMAND:
    case MSP_MULTIPLE_MSP:
    case MSP_REBOOT:
        return false;
    default:
        if (!bbp.active) {
            return false;
        }
        blackboxParamsOpBegin(BBP_SRC_MSP, cmd);
        return true;
    }
}

static uint32_t runtimeValue(int i)
{
    switch (i) {
    case BBP_RT_GOV_MODE:
        return getGovernorMode();
    case BBP_RT_FEATURES:
        return featureRuntimeMask();
    case BBP_RT_FP_PID:
        return pidParamFingerprint();
    case BBP_RT_FP_GOV:
        return governorParamFingerprint();
    default:
        return setpointParamFingerprint();
    }
}

void blackboxParamsApplied(bbpLoader_e loader, int slot)
{
    if (!bbp.active) {
        return;
    }
    const bool outside = (bbp.opDepth == 0);
    if (outside) {
        blackboxParamsOpBegin('l', loader);
    }

    // A profile that is not in the group of profiles: the whole group
    const int slotGroup = bbpLoaderSlotGroup(loader);
    if (slotGroup < 0 || slot >= bbpPg[slotGroup].reg->length) {
        slot = -1;
    }

    // The writes that the loader read come before its 'A' record
    const bbpPoint_t at = pointNow();
    capture(true, bbp.opSrc, bbp.opArg, at, bbpLoaderRegion[loader], slot, slotGroup);

    uint32_t fp;
    int count = 1;
    switch (loader) {
    case BBP_LOADER_PID:
        fp = runtimeValue(BBP_RT_FP_PID);
        break;
    case BBP_LOADER_GOVERNOR:
        fp = runtimeValue(BBP_RT_FP_GOV);
        break;
    case BBP_LOADER_SETPOINT:
        fp = runtimeValue(BBP_RT_FP_SP);
        break;
    case BBP_LOADER_FEATURE:
        fp = runtimeValue(BBP_RT_FEATURES);
        break;
    default:
        fp = 0;
        count = 0;
        break;
    }
    recValues('A', loader, (slot >= 0) ? slot : BBP_ARG_NONE, at, at, &fp, count);

    if (outside) {
        blackboxParamsOpEnd();
    }
}

void blackboxParamsMarker(bbpMarker_e marker, uint32_t a, uint32_t b)
{
    if (bbp.active) {
        const bbpPoint_t at = pointNow();
        const uint32_t values[2] = { a, b };
        recValues('M', marker, BBP_ARG_NONE, at, at, values, 2);
    }
}

bool blackboxParamsActive(void)
{
    return bbp.active;
}

/* Events */

static void putPoint(bbpWriter_t *w, bbpPoint_t point)
{
    bbpPutUint(w, point.n);
    bbpPutChar(w, '.');
    bbpPutUint(w, point.c);
}

static void putAt(bbpWriter_t *w, bbpPoint_t from, bbpPoint_t at)
{
    if (at.n == BBP_PRE) {
        bbpPutChar(w, 'p');
        return;
    }
    if (from.n != at.n || from.c != at.c) {
        putPoint(w, from);
        bbpPutChar(w, '~');
    }
    putPoint(w, at);
}

static void putHex8(bbpWriter_t *w, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        bbpPutChar(w, digits[(value >> shift) & 15]);
    }
}

// CRC-8 with polynomial 0xD5 (crc8_dvb_s2_update) a nibble at a time: the PID task formats the events
static uint8_t eventCrc(const char *p, int length)
{
    static const uint8_t table[16] = {
        0x00, 0xD5, 0x7F, 0xAA, 0xFE, 0x2B, 0x81, 0x54, 0x29, 0xFC, 0x56, 0x83, 0xD7, 0x02, 0xA8, 0x7D
    };
    uint8_t crc = 0;

    for (int i = 0; i < length; i++) {
        crc ^= (uint8_t)p[i];
        crc = (uint8_t)(crc << 4) ^ table[crc >> 4];
        crc = (uint8_t)(crc << 4) ^ table[crc >> 4];
    }
    return crc;
}

// "*<crc>" and the end of the string
static int endEvent(bbpWriter_t *w, char *line)
{
    static const char digits[] = "0123456789ABCDEF";
    const int length = w->ptr - line;
    const uint8_t crc = eventCrc(line, length);

    w->end = line + BBP_EVENT_MAX;
    bbpPutChar(w, '*');
    bbpPutChar(w, digits[crc >> 4]);
    bbpPutChar(w, digits[crc & 15]);
    *w->ptr = 0;
    return w->ptr - line;
}

static bool lostBitNext(uint32_t mask, uint16_t *bit)
{
    while (*bit < BBP_TRACKED && !(mask & BIT(*bit))) {
        (*bit)++;
    }
    return *bit < BBP_TRACKED;
}

// The next event of the record at the ring tail, in bbpLine
static void formatEvent(void)
{
    const uint16_t h = bbp.ringTail;
    const uint8_t type = ringGet(h + REC_TYPE);
    const uint8_t src = ringGet(h + REC_SRC);
    const uint16_t arg = ringGetN(h + REC_ARG, 2);
    const bbpPoint_t from = { ringGetN(h + REC_N0, 4), ringGet(h + REC_C0) };
    const bbpPoint_t at = { ringGetN(h + REC_N1, 4), ringGet(h + REC_C1) };
    const uint16_t payload = h + BBP_REC_HEADER;
    const uint16_t end = payload + ringGetN(h + REC_LEN, 2);

    // Room for "*XX"
    bbpWriter_t w = { bbpLine, bbpLine + BBP_EVENT_MAX - 3, false };

    bbpPutChar(&w, 'P');
    bbpPutChar(&w, bbp.drainStarted ? '+' : type);
    bbpPutHex(&w, ringGetN(h + REC_SEQ, 4));
    bbpPutChar(&w, ' ');
    putAt(&w, from, at);

    uint16_t pos = bbp.drainStarted ? bbp.drainPos : payload;
    bool done = true;

    switch (type) {
    case 'C': {
        if (!bbp.drainStarted) {
            bbpPutStr(&w, " s=");
            bbpPutChar(&w, src);
            if (src == 'y') {
                bbpPutStr(&w, " pgs=");
                bbpPutUint(&w, arg);
            } else if (arg != BBP_ARG_NONE) {
                bbpPutChar(&w, '.');
                bbpPutUint(&w, arg);
            }
            bbpPutStr(&w, " n=");
            bbpPutUint(&w, ringGetN(h + REC_COUNT, 2));
        }
        int placed = 0;
        while (pos != end) {
            uint32_t newBytes[BBP_ITEM_BYTES_MAX / 4];
            uint32_t oldBytes[BBP_ITEM_BYTES_MAX / 4];
            const uint16_t code = ringGetN(pos, 2);
            const uint8_t slot = ringGet(pos + 2);
            const uint8_t index = ringGet(pos + 3);
            const uint8_t length = MIN(ringGet(pos + 4), BBP_ITEM_BYTES_MAX);
            for (unsigned i = 0; i < length; i++) {
                ((uint8_t *)newBytes)[i] = ringGet(pos + 5 + i);
                ((uint8_t *)oldBytes)[i] = ringGet(pos + 5 + length + i);
            }
            char *mark = w.ptr;
            bbpPutChar(&w, ' ');
            if (!bbpPutItem(&w, code, slot, index, (uint8_t *)newBytes, (uint8_t *)oldBytes, length)) {
                w.ptr = mark;
                w.overflow = false;
                if (placed == 0 && bbp.drainStarted) {
                    // Cannot happen: an item fits in a continuation. Do not stall on it.
                    pos += 5 + 2 * length;
                    bbp.lostRec++;
                    continue;
                }
                break;
            }
            placed++;
            pos += 5 + 2 * length;
        }
        done = (pos == end);
        break;
    }
    case 'A':
        bbpPutChar(&w, ' ');
        bbpPutStr(&w, bbpLoaderNames[src < BBP_LOADER_COUNT ? src : 0]);
        if (arg != BBP_ARG_NONE) {
            bbpPutChar(&w, '/');
            bbpPutUint(&w, arg);
        }
        if (pos != end) {
            bbpPutStr(&w, " fp=");
            putHex8(&w, ringGetN(pos, 4));
        }
        break;
    case 'R': {
        const uint8_t mask = src;
        for (int i = 0; i < BBP_RT_COUNT; i++) {
            if (mask & BIT(i)) {
                const uint32_t value = ringGetN(pos, 4);
                pos += 4;
                bbpPutChar(&w, ' ');
                bbpPutStr(&w, runtimeNames[i]);
                if (i == BBP_RT_GOV_MODE) {
                    bbpPutUint(&w, value);
                } else if (i == BBP_RT_FEATURES) {
                    bbpPutHex(&w, value);
                } else {
                    putHex8(&w, value);
                }
            }
        }
        break;
    }
    case 'M': {
        const uint32_t a = ringGetN(pos, 4);
        const uint32_t b = ringGetN(pos + 4, 4);
        bbpPutChar(&w, ' ');
        bbpPutStr(&w, markerNames[src < BBP_MARKER_COUNT ? src : 0]);
        if (src == BBP_MARKER_EESAVE) {
            bbpPutStr(&w, " us=");
            bbpPutUint(&w, a);
        } else if (src == BBP_MARKER_ESCPARAM) {
            bbpPutStr(&w, " n=");
            bbpPutUint(&w, a);
            bbpPutStr(&w, " fnv=");
            putHex8(&w, b);
        }
        break;
    }
    case 'L': {
        // drainPos: the next group of the list
        const uint32_t mask = ringGetN(payload, 4);
        uint16_t bit = bbp.drainStarted ? bbp.drainPos : 0;
        bbpPutStr(&w, " pgs=");
        bool first = true;
        while (lostBitNext(mask, &bit)) {
            char *mark = w.ptr;
            if (!first) {
                bbpPutChar(&w, ',');
            }
            bbpPutUint(&w, pgN(bbpPg[bit].reg));
            if (w.overflow) {
                w.ptr = mark;
                w.overflow = false;
                break;
            }
            first = false;
            bit++;
        }
        done = !lostBitNext(mask, &bit);
        pos = bit;
        break;
    }
    default:
        break;
    }

    bbp.eventLength = endEvent(&w, bbpLine);
    bbp.drainNext = pos;
    bbp.drainDone = done;
    bbp.eventReady = true;
}

// At most one event in each logged iteration, and only when BBP_FRAME_RESERVE bytes stay free for frames
static void drainOneEvent(void)
{
    if (!bbp.eventReady) {
        if (bbp.ringTail == bbp.ringHead) {
            return;
        }
        formatEvent();
    }

    // 'E', the event, the length and the chars
    if (blackboxDeviceFreeSpace() < bbp.eventLength + 3 + BBP_FRAME_RESERVE) {
        return;
    }
    blackboxLogCustomString(bbpLine);

    bbp.eventReady = false;
    if (bbp.drainDone) {
        bbp.ringTail += BBP_REC_HEADER + ringGetN(bbp.ringTail + REC_LEN, 2);
        bbp.drainStarted = false;
    } else {
        bbp.drainPos = bbp.drainNext;
        bbp.drainStarted = true;
    }
}

/* PID task */

static void writeLost(void)
{
    const uint32_t mask = bbp.lostMask & ~bbp.lostListed;
    // The other records keep BBP_RING_KEEP bytes free for it
    if (ringFree() < BBP_REC_HEADER + 4) {
        return;
    }
    if (recBegin('L', 0, BBP_ARG_NONE, bbp.lostAt, bbp.lostAt)) {
        recPutN(4, mask);
        recCommit();
        bbp.lostListed |= mask;
    }
}

static void resyncOneGroup(bbpPoint_t now)
{
    for (int t = 0; t < BBP_TRACKED; t++) {
        if (bbp.lostMask & BIT(t)) {
            capture(false, 'y', BBP_ARG_NONE, now, BIT(t), -1, -1);
            return;
        }
    }
}

/*
 * Compare BBP_SCAN_BYTES of one group with the shadow. A group that is larger than the step is compared over
 * more than one logged iteration: its verified point becomes the point at which the pass started.
 * A difference is a write that no hook saw: a 'u' record with an interval.
 */
static void scanStep(bbpPoint_t now)
{
    for (int i = 0; i < BBP_TRACKED && !(bbp.allMask & BIT(bbp.scanPg)); i++) {
        bbp.scanPg = (bbp.scanPg + 1) % BBP_TRACKED;
        bbp.scanOff = 0;
    }
    const int t = bbp.scanPg;
    if (!(bbp.allMask & BIT(t))) {
        return;
    }

    if (bbp.scanOff == 0) {
        bbp.scanPassStart = now;
    }
    const unsigned size = pgSize(bbpPg[t].reg);
    const unsigned end = MIN(bbp.scanOff + (unsigned)BBP_SCAN_BYTES, size);

    if (groupDiffers(t, bbp.scanOff, end)) {
        capture(false, 'u', BBP_ARG_NONE, now, BIT(t), -1, -1);
        bbp.scanOff = size;
    } else if (end >= size) {
        if (pointAfter(bbp.scanPassStart, bbpVerified[t])) {
            bbpVerified[t] = bbp.scanPassStart;
        }
        bbp.scanOff = size;
    } else {
        bbp.scanOff = end;
    }

    if (bbp.scanOff >= size) {
        bbp.scanPg = (t + 1) % BBP_TRACKED;
        bbp.scanOff = 0;
    }
}

// Poll one runtime value: an 'R' record when it changed since the last poll
static void pollOneRuntime(bbpPoint_t now)
{
    const int i = bbp.rtIndex;
    const uint32_t value = runtimeValue(i);

    if (value != bbp.rtLast[i]) {
        // Only when it fits: else the next poll tries again
        if (ringFree() < BBP_REC_HEADER + 4 + BBP_RING_KEEP) {
            return;
        }
        recValues('R', BIT(i), BBP_ARG_NONE, bbp.rtPolled[i], now, &value, 1);
        bbp.rtLast[i] = value;
    }
    bbp.rtPolled[i] = now;
    bbp.rtIndex = (i + 1) % BBP_RT_COUNT;
}

void blackboxParamsAfterFrame(void)
{
    if (!bbp.active || !bbp.t0) {
        return;
    }
    const bbpPoint_t now = pointAfterFrame();

    if (bbp.lostMask & ~bbp.lostListed) {
        writeLost();
    }

    drainOneEvent();

    if (bbp.lostMask && ringFree() >= BBP_RESYNC_FREE) {
        resyncOneGroup(now);
    } else if (!bbp.lostMask) {
        scanStep(now);
    }

    if ((++bbp.rtTick & 3) == 0) {
        pollOneRuntime(now);
    }
}

/* Log lifecycle */

void blackboxParamsBoot(void)
{
    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = bbpPg[t].reg;
        if (reg) {
            if (bbpTrackedPgs[t].flags & BBP_PG_BOOT) {
                memcpy(bbpBootCopy + bbpBootOffset(t), reg->address, pgSize(reg));
            }
            copyToShadow(t);
        }
    }
    bbp.shadowValid = true;
}

void blackboxParamsShadowLost(void)
{
#ifndef BLACKBOX_PARAMS_OWN_SHADOW
    // The journal stops: the CLI stops the log, and leaves by a reboot
    bbp.shadowValid = false;
    bbp.active = false;
#endif
}

static void clearJournal(void)
{
    bbp.t0 = false;
    bbp.opDepth = 0;
    bbp.seq = 0;
    bbp.prevMask = 0;
    bbp.lostMask = 0;
    bbp.lostListed = 0;
    bbp.lostRec = 0;
    bbp.ringHead = 0;
    bbp.ringTail = 0;
    bbp.eventReady = false;
    bbp.drainStarted = false;
    bbp.holding = false;
    rec.open = false;
}

void blackboxParamsStart(void)
{
    const uint8_t mode = blackboxConfig()->params;

    clearJournal();
    bbp.active = (mode != BLACKBOX_PARAMS_OFF);
    bbp.lineLength = 0;
    bbp.linePos = 0;

    if (!bbp.active) {
        return;
    }

    for (int t = 0; t < BBP_TRACKED; t++) {
        bbpVerified[t] = pointPre;
    }

    if (bbp.shadowValid) {
        // Compared one group in each header iteration: 'v' records
        bbp.prevMask = bbp.allMask;
    } else {
        for (int t = 0; t < BBP_TRACKED; t++) {
            if (bbpPg[t].reg) {
                copyToShadow(t);
            }
        }
        bbp.shadowValid = true;
        const uint32_t values[2] = { 0, 0 };
        recValues('M', BBP_MARKER_SHADOW_RESET, BBP_ARG_NONE, pointPre, pointPre, values, 2);
    }

    bbpHeaderBegin(mode != BLACKBOX_PARAMS_CHANGES);
}

void blackboxParamsHeaderTick(void)
{
    if (bbp.active && bbp.prevMask) {
        capture(false, 'v', BBP_ARG_NONE, pointPre, bbp.prevMask & -bbp.prevMask, -1, -1);
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

void blackboxParamsRunning(void)
{
    if (!bbp.active) {
        return;
    }
    if (bbp.prevMask) {
        capture(false, 'v', BBP_ARG_NONE, pointPre, bbp.prevMask, -1, -1);
    }

    // The base of the polls
    uint32_t values[BBP_RT_COUNT];
    const bbpPoint_t first = { 0, 0 };
    for (int i = 0; i < BBP_RT_COUNT; i++) {
        values[i] = runtimeValue(i);
        bbp.rtLast[i] = values[i];
        bbp.rtPolled[i] = first;
    }
    recValues('R', BIT(BBP_RT_COUNT) - 1, BBP_ARG_NONE, pointPre, pointPre, values, BBP_RT_COUNT);
    bbp.rtIndex = 0;
    bbp.rtTick = 0;

    bbp.scanPg = 0;
    bbp.scanOff = 0;
    bbp.t0 = true;
}

bool blackboxParamsHoldLogEnd(void)
{
    if (!bbp.active || !bbp.t0 || bbp.ringTail == bbp.ringHead) {
        return false;
    }
    // A new hold when the last request was not in the previous iteration (the log went on)
    const uint32_t iteration = blackboxGetIteration();
    if (!bbp.holding || iteration - bbp.holdIteration > 1) {
        bbp.holding = true;
        bbp.holdStart = millis();
    }
    bbp.holdIteration = iteration;
    return cmp32(millis(), bbp.holdStart) < BBP_HOLD_MS;
}

void blackboxParamsEnd(void)
{
    if (!bbp.active || !bbp.t0) {
        return;
    }

    // Records that are not completely written
    unsigned unsent = 0;
    for (uint16_t h = bbp.ringTail; h != bbp.ringHead; h += BBP_REC_HEADER + ringGetN(h + REC_LEN, 2)) {
        unsent++;
    }

    const bbpPoint_t at = { blackboxGetIteration(), 0 };
    bbpWriter_t w = { bbpLine, bbpLine + BBP_EVENT_MAX - 3, false };
    bbpPutStr(&w, "PQ");
    bbpPutHex(&w, ++bbp.seq);
    bbpPutChar(&w, ' ');
    putAt(&w, at, at);
    bbpPutStr(&w, " unsent=");
    bbpPutUint(&w, unsent);
    bbpPutStr(&w, " lostrec=");
    bbpPutUint(&w, bbp.lostRec);
    endEvent(&w, bbpLine);
    blackboxLogCustomString(bbpLine);
}

void blackboxParamsStop(void)
{
    clearJournal();
    bbp.active = false;
}

uint32_t blackboxParamsSeq(void)
{
    return bbp.seq;
}

#endif
