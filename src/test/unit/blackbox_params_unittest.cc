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
 * Parameter log (blackbox_params): value formatter and header section.
 *
 * - The formatter gives the text of CLI "get" for every valueTable entry, compared with printValuePointer() of
 *   cli.c (copied from cli.c at build time) for random bytes.
 * - The header section is decoded back into parameter group bytes. Every byte of every tracked group must
 *   come back, from exactly one kind of line.
 * - The line rules: one line per call, the header budget, the line length, param_end count and hash.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <vector>

extern "C" {
    #include "platform.h"

    #include "common/crc.h"
    #include "common/utils.h"

    #include "cli/settings.h"

    #include "pg/pg.h"
    #include "pg/pg_ids.h"
    #include "pg/blackbox.h"
    #include "pg/feature.h"
    #include "pg/governor.h"
    #include "pg/pid.h"
    #include "pg/rates.h"
    #include "pg/servos.h"
    #include "pg/system.h"

    #include "fc/core.h"

    #include "sensors/gyro.h"

    #include "blackbox/blackbox_io.h"
    #include "blackbox/blackbox_params.h"
    #include "blackbox/blackbox_params_impl.h"

    #include "blackbox_params_unittest_cli.h"

    // Stubs of the firmware
    uint8_t activePidLoopDenom = 2;
    uint8_t activeFilterLoopDenom = 2;
    gyro_t gyro;
    pidProfile_t *currentPidProfile;
    controlRateConfig_t *currentControlRateProfile;
    int32_t blackboxHeaderBudget;

    void blackboxWrite(uint8_t value);
    uint32_t blackboxGetPInterval(void);
    uint8_t coreSubtaskTick(coreSubtask_e subtask);
    int getGovernorMode(void);
    uint32_t featureRuntimeMask(void);
    uint8_t getMotorCount(void);
    uint8_t getServoCount(void);
}

#include "unittest_macros.h"
#include "gtest/gtest.h"

static std::string deviceBytes;

void blackboxWrite(uint8_t value) { deviceBytes.push_back(value); }
uint32_t blackboxGetPInterval(void) { return 8; }
uint8_t coreSubtaskTick(coreSubtask_e subtask)
{
    // pid_process_denom 2 (core.c)
    static const uint8_t ticks[CORE_ST_COUNT] = { 0, 0, 0, 1, 1, 1, 1, 0 };
    return ticks[subtask];
}
int getGovernorMode(void) { return 0; }
uint32_t featureRuntimeMask(void) { return featureConfig()->enabledFeatures; }
uint8_t getMotorCount(void) { return 1; }
uint8_t getServoCount(void) { return 4; }

/* Helpers */

static std::mt19937 rng(20261006);

static const pgRegistry_t *trackedReg(int t) { return bbpPg[t].reg; }

static void setupConfig(uint8_t mode)
{
    gyro.targetLooptime = 250;
    pgResetAll();
    blackboxConfigMutable()->params = mode;
    currentPidProfile = pidProfilesMutable(0);
    currentControlRateProfile = controlRateProfilesMutable(0);
    blackboxParamsInit();
    blackboxParamsBoot();
}

static void fillRandom(uint8_t *p, int count)
{
    for (int i = 0; i < count; i++) {
        p[i] = rng();
    }
}

// Strings stop at the first NUL, as the CLI and MSP write them: zero the bytes after it
static void tidyStrings(void)
{
    for (int i = 0; i < valueTableEntryCount; i++) {
        const clivalue_t *v = &valueTable[i];
        const int t = bbpTrackedIndex(v->pgn);
        if (t < 0 || (v->type & VALUE_MODE_MASK) != MODE_STRING) {
            continue;
        }
        const pgRegistry_t *reg = trackedReg(t);
        const int section = v->type & VALUE_SECTION_MASK;
        const int instances = (section == PROFILE_VALUE || section == PROFILE_RATE_VALUE) ? reg->length : 1;
        for (int k = 0; k < instances; k++) {
            char *s = (char *)reg->address + k * pgElementSize(reg) + v->offset;
            bool end = false;
            for (int c = 0; c < v->config.string.maxlength; c++) {
                end = end || !s[c];
                if (end) {
                    s[c] = 0;
                }
            }
        }
    }
}

struct HeaderRun {
    std::string text;
    int calls;
};

// The calls of blackboxWriteSysinfo(): the budget grows by budgetPerCall up to 256 (blackboxReplenishHeaderBudget),
// and a call is made only with 64 B of budget (blackboxDeviceReserveBufferSpace(64))
static HeaderRun writeHeader(int budgetPerCall = 64)
{
    HeaderRun run = { "", 0 };
    deviceBytes.clear();
    blackboxHeaderBudget = 0;
    blackboxParamsStart();
    for (int iteration = 0; iteration < 1000000; iteration++) {
        blackboxHeaderBudget = MIN(blackboxHeaderBudget + budgetPerCall, 256);
        if (blackboxHeaderBudget < 64) {
            continue;
        }
        const size_t before = deviceBytes.size();
        const int32_t budget = blackboxHeaderBudget;
        const bool done = blackboxParamsWriteHeader();
        run.calls++;
        const std::string chunk = deviceBytes.substr(before);
        EXPECT_LE((int)chunk.size(), budget);
        EXPECT_LE((int)chunk.size(), BLACKBOX_TARGET_HEADER_BUDGET_PER_ITERATION);
        EXPECT_EQ(budget - (int)chunk.size(), blackboxHeaderBudget);
        // At most one line in one call: a newline can only be the last byte
        const size_t newline = chunk.find('\n');
        EXPECT_TRUE(newline == std::string::npos || newline == chunk.size() - 1) << chunk;
        if (done) {
            EXPECT_EQ(0u, chunk.size());
            break;
        }
    }
    run.text = deviceBytes;
    return run;
}

static std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        EXPECT_NE(std::string::npos, end) << "last line without newline";
        if (end == std::string::npos) {
            break;
        }
        lines.push_back(text.substr(start, end - start + 1));
        start = end + 1;
    }
    return lines;
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t end = s.find(sep, start);
        parts.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

static int typeSizeOf(const clivalue_t *v)
{
    switch (v->type & VALUE_TYPE_MASK) {
    case VAR_UINT16:
    case VAR_INT16:
        return 2;
    case VAR_UINT32:
        return 4;
    default:
        return 1;
    }
}

static void storeNumber(uint8_t *p, int size, int64_t value)
{
    const uint32_t u = (uint32_t)value;
    memcpy(p, &u, size);    // little endian
}

static int64_t readNumber(const clivalue_t *v, const uint8_t *p)
{
    switch (v->type & VALUE_TYPE_MASK) {
    case VAR_INT8: return *(const int8_t *)p;
    case VAR_UINT16: { uint16_t x; memcpy(&x, p, 2); return x; }
    case VAR_INT16: { int16_t x; memcpy(&x, p, 2); return x; }
    case VAR_UINT32: { uint32_t x; memcpy(&x, p, 4); return x; }
    default: return *p;
    }
}

static bool parseInteger(const std::string &s, int64_t *value, int base = 10)
{
    if (s.empty()) {
        return false;
    }
    char *end;
    *value = strtoll(s.c_str(), &end, base);
    return *end == 0;
}

static std::string unescape(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            r.push_back((char)strtol(s.substr(i + 1, 2).c_str(), NULL, 16));
            i += 2;
        } else {
            r.push_back(s[i]);
        }
    }
    return r;
}

// The bytes of a value from its header text. Bitsets set their bit only.
static bool parseValue(const clivalue_t *v, const std::string &text, uint8_t *p)
{
    const int size = typeSizeOf(v);
    int64_t value;

    switch (v->type & VALUE_MODE_MASK) {
    case MODE_ARRAY: {
        const std::vector<std::string> parts = split(text, ',');
        if ((int)parts.size() != v->config.array.length) {
            return false;
        }
        for (int i = 0; i < v->config.array.length; i++) {
            if (!parseInteger(parts[i], &value)) {
                return false;
            }
            storeNumber(p + i * size, size, value);
        }
        return true;
    }
    case MODE_LOOKUP:
        if (text[0] == '?') {
            if (!parseInteger(text.substr(1), &value)) {
                return false;
            }
        } else {
            const lookupTableEntry_t *table = &lookupTables[v->config.lookup.tableIndex];
            value = -1;
            for (int i = 0; i < table->valueCount; i++) {
                if (text == table->values[i]) {
                    value = i;
                    break;
                }
            }
            if (value < 0) {
                return false;
            }
        }
        storeNumber(p, size, value);
        return true;
    case MODE_BITSET: {
        int64_t word = readNumber(v, p);
        if (text == "ON") {
            word |= (1LL << v->config.bitpos);
        } else if (text == "OFF") {
            word &= ~(1LL << v->config.bitpos);
        } else {
            return false;
        }
        storeNumber(p, size, word);
        return true;
    }
    case MODE_STRING: {
        const std::string s = (text == "-") ? "" : unescape(text);
        if ((int)s.size() > v->config.string.maxlength) {
            return false;
        }
        memset(p, 0, v->config.string.maxlength);
        memcpy(p, s.data(), s.size());
        return true;
    }
    default:
        if (!parseInteger(text, &value)) {
            return false;
        }
        storeNumber(p, size, value);
        return true;
    }
}

static int fieldSize(const bbpField_t *field)
{
    return (field->type == BBP_F_U16 || field->type == BBP_F_S16) ? 2 : (field->type == BBP_F_HEX32) ? 4 : 1;
}

/*
 * Decoder: the parameter group bytes from the header section
 */
struct Decoded {
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<std::vector<uint8_t>> known;    // 1: from a set or element line, 2: from a pg line
    std::map<std::string, std::string> keys;    // all keys and values
    std::vector<std::string> keyOrder;
    std::vector<std::string> errors;
    int lines = 0;
    uint32_t hash = FNV_OFFSET_BASIS;
    bool endSeen = false;
    unsigned endLines = 0;
    uint32_t endHash = 0;
};

static const clivalue_t *findEntry(const std::string &name, int sectionClass)
{
    for (int i = 0; i < valueTableEntryCount; i++) {
        const clivalue_t *v = &valueTable[i];
        int section = v->type & VALUE_SECTION_MASK;
        if (section == HARDWARE_VALUE) {
            section = MASTER_VALUE;
        }
        if (section == sectionClass && name == v->name) {
            return v;
        }
    }
    return NULL;
}

static void markKnown(Decoded &d, int t, unsigned offset, unsigned count, uint8_t how)
{
    for (unsigned i = offset; i < offset + count; i++) {
        if (d.known[t][i] && how == 2) {
            d.errors.push_back("raw byte also given by a set or element line: pg " + std::to_string(pgN(trackedReg(t))) + "+" + std::to_string(i));
        }
        d.known[t][i] = how;
    }
}

static void applyValue(Decoded &d, const clivalue_t *v, int instance, const std::string &text)
{
    const int t = bbpTrackedIndex(v->pgn);
    if (t < 0) {
        d.errors.push_back(std::string("untracked entry ") + v->name);
        return;
    }
    const unsigned offset = instance * pgElementSize(trackedReg(t)) + v->offset;
    if (!parseValue(v, text, &d.bytes[t][offset])) {
        d.errors.push_back(std::string("bad value ") + v->name + ":" + text);
        return;
    }
    if (bbpValueWidth(v) > 0) {
        markKnown(d, t, offset, bbpValueWidth(v), 1);
    }
}

static Decoded decodeHeader(const std::string &text)
{
    Decoded d;
    for (int t = 0; t < BBP_TRACKED; t++) {
        const int size = trackedReg(t) ? pgSize(trackedReg(t)) : 0;
        d.bytes.push_back(std::vector<uint8_t>(size, 0));
        d.known.push_back(std::vector<uint8_t>(size, 0));
    }

    for (const std::string &line : splitLines(text)) {
        if (line.size() > BBP_LINE_MAX) {
            d.errors.push_back("line too long: " + line);
        }
        if (line.compare(0, 2, "H ") != 0) {
            d.errors.push_back("not a header line: " + line);
            continue;
        }
        const size_t colon = line.find(':');
        const std::string key = line.substr(2, colon - 2);
        const std::string value = line.substr(colon + 1, line.size() - colon - 2);
        if (d.keys.count(key)) {
            d.errors.push_back("key twice: " + key);
        }
        d.keys[key] = value;
        d.keyOrder.push_back(key);

        if (key == "param_end") {
            d.endSeen = true;
            const std::vector<std::string> parts = split(value, ',');
            d.endLines = strtoul(parts[0].c_str(), NULL, 10);
            d.endHash = strtoul(parts[1].c_str(), NULL, 16);
            if (parts[1].size() != 8) {
                d.errors.push_back("hash width");
            }
            continue;
        }
        if (d.endSeen) {
            d.errors.push_back("line after param_end: " + line);
        }
        d.lines++;
        d.hash = fnv_update(d.hash, line.data(), line.size());

        if (key.compare(0, 4, "set.") == 0) {
            const clivalue_t *v = findEntry(key.substr(4), MASTER_VALUE);
            if (!v) {
                d.errors.push_back("unknown " + key);
                continue;
            }
            applyValue(d, v, 0, value);
        } else if (key.compare(0, 4, "set@") == 0) {
            const int section = (key[4] == 'p') ? PROFILE_VALUE : PROFILE_RATE_VALUE;
            const size_t dot = key.find('.');
            const clivalue_t *v = findEntry(key.substr(dot + 1), section);
            if (!v) {
                d.errors.push_back("unknown " + key);
                continue;
            }
            if (dot == 5) {
                const std::vector<std::string> fields = split(value, '|');
                const int t = bbpTrackedIndex(v->pgn);
                if (t < 0 || (int)fields.size() != trackedReg(t)->length) {
                    d.errors.push_back("profile count " + key);
                    continue;
                }
                std::string previous;
                for (size_t k = 0; k < fields.size(); k++) {
                    if (k == 0 && fields[k].empty()) {
                        d.errors.push_back("empty first profile " + key);
                    }
                    if (!fields[k].empty()) {
                        previous = fields[k];
                    }
                    applyValue(d, v, k, previous);
                }
            } else {
                applyValue(d, v, atoi(key.substr(5, dot - 5).c_str()), value);
            }
        } else if (key.compare(0, 3, "el.") == 0) {
            const std::vector<std::string> parts = split(key, '.');
            const bbpElementKind_t *kind = NULL;
            for (int k = 0; k < BBP_ELEMENT_KINDS; k++) {
                if (parts[1] == bbpElementKinds[k].name) {
                    kind = &bbpElementKinds[k];
                }
            }
            const int t = kind ? bbpTrackedIndex(kind->pgn) : -1;
            if (t < 0) {
                d.errors.push_back("unknown " + key);
                continue;
            }
            unsigned first = 0, last = 0;
            if (kind->indexed) {
                const std::vector<std::string> range = split(parts[2], '-');
                first = last = strtoul(range[0].c_str(), NULL, 10);
                if (range.size() > 1) {
                    last = strtoul(range[1].c_str(), NULL, 10);
                    if (last <= first) {
                        d.errors.push_back("bad range " + key);
                    }
                }
            }
            const std::vector<std::string> fields = split(value, ',');
            if ((int)fields.size() != kind->fieldCount) {
                d.errors.push_back("field count " + key);
                continue;
            }
            const unsigned size = pgElementSize(trackedReg(t));
            for (unsigned i = first; i <= last; i++) {
                for (int f = 0; f < kind->fieldCount; f++) {
                    int64_t number;
                    if (!parseInteger(fields[f], &number, kind->fields[f].type == BBP_F_HEX32 ? 16 : 10)) {
                        d.errors.push_back("bad field " + key);
                    }
                    storeNumber(&d.bytes[t][i * size + kind->fields[f].offset], fieldSize(&kind->fields[f]), number);
                    markKnown(d, t, i * size + kind->fields[f].offset, fieldSize(&kind->fields[f]), 1);
                }
            }
        } else if (key.compare(0, 3, "pg.") == 0) {
            const size_t plus = key.find('+');
            const int t = bbpTrackedIndex(atoi(key.substr(3, plus - 3).c_str()));
            const unsigned offset = atoi(key.substr(plus + 1).c_str());
            if (t < 0 || value.size() % 2 || value.size() / 2 > BBP_RAW_MAX || offset + value.size() / 2 > d.bytes[t].size()) {
                d.errors.push_back("bad raw " + line);
                continue;
            }
            for (size_t i = 0; i < value.size() / 2; i++) {
                d.bytes[t][offset + i] = strtoul(value.substr(2 * i, 2).c_str(), NULL, 16);
            }
            markKnown(d, t, offset, value.size() / 2, 2);
        }
    }
    return d;
}

// The decoded bytes equal the parameter groups, and each byte is given once
static void expectExact(const Decoded &d)
{
    for (const std::string &e : d.errors) {
        ADD_FAILURE() << e;
    }
    EXPECT_TRUE(d.endSeen);
    EXPECT_EQ((unsigned)d.lines, d.endLines);
    EXPECT_EQ(d.hash, d.endHash);

    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = trackedReg(t);
        if (!reg) {
            continue;
        }
        int unknown = 0, wrong = 0;
        for (unsigned i = 0; i < pgSize(reg); i++) {
            unknown += !d.known[t][i];
            if (d.known[t][i] && d.bytes[t][i] != reg->address[i]) {
                wrong++;
                ADD_FAILURE() << "pg " << pgN(reg) << "+" << i << ": " << (int)d.bytes[t][i] << " instead of " << (int)reg->address[i];
            }
        }
        EXPECT_EQ(0, unknown) << "pg " << pgN(reg) << ": bytes not in the header";
        EXPECT_EQ(0, wrong) << "pg " << pgN(reg) << ": bytes that differ";
    }
}

static int countKeys(const Decoded &d, const char *prefix)
{
    int n = 0;
    for (const std::string &key : d.keyOrder) {
        n += key.compare(0, strlen(prefix), prefix) == 0;
    }
    return n;
}

/* Tests */

TEST(BlackboxParamsTest, FormatterMatchesCli)
{
    alignas(4) uint8_t buf[64];
    char cli[512];
    char text[512];
    int compared = 0;

    for (int i = 0; i < valueTableEntryCount; i++) {
        const clivalue_t *v = &valueTable[i];
        for (int n = 0; n < 400; n++) {
            if (n == 0) {
                memset(buf, 0, sizeof(buf));
            } else if (n == 1) {
                memset(buf, 0xFF, sizeof(buf));
            } else if (n == 2) {
                memset(buf, 0x80, sizeof(buf));
            } else {
                fillRandom(buf, sizeof(buf));
            }
            if ((v->type & VALUE_MODE_MASK) == MODE_STRING) {
                // The CLI prints up to the NUL
                const int max = v->config.string.maxlength;
                buf[rng() % (max + 1)] = 0;
                buf[max] = 0;
            }

            bbpWriter_t w = { text, text + sizeof(text) - 1, false };
            bbpPutValue(&w, v, buf);
            *w.ptr = 0;
            ASSERT_FALSE(w.overflow);
            EXPECT_LE((int)strlen(text), bbpValueMaxLength(v)) << v->name << " " << text;

            const bool lookup = (v->type & VALUE_MODE_MASK) == MODE_LOOKUP;
            const int64_t value = readNumber(v, buf);
            if (lookup && value < 0) {
                // The CLI reads before the lookup table: compare with the rule only
                EXPECT_EQ("?" + std::to_string(value), std::string(text)) << v->name;
                continue;
            }
            const bool valid = cliTestPrintValue(cli, v, buf);
            if (lookup && !valid) {
                // Out of range: the CLI prints no name
                EXPECT_EQ("?" + std::to_string(value), std::string(text)) << v->name;
            } else if ((v->type & VALUE_MODE_MASK) == MODE_STRING) {
                EXPECT_EQ(std::string(cli), unescape(text)) << v->name;
                for (const char *c = text; *c; c++) {
                    EXPECT_TRUE(*c > ' ' && *c < 0x7F && !strchr("|,<>&*=:", *c)) << v->name << " " << text;
                }
            } else {
                EXPECT_STREQ(cli, text) << v->name;
            }
            compared++;
        }
    }
    printf("formatter: %d valueTable entries, %d comparisons with the CLI\n", valueTableEntryCount, compared);
}

TEST(BlackboxParamsTest, OffWritesNothing)
{
    setupConfig(BLACKBOX_PARAMS_OFF);
    deviceBytes.clear();
    blackboxHeaderBudget = 256;
    blackboxParamsStart();
    EXPECT_TRUE(blackboxParamsWriteHeader());
    EXPECT_EQ(0u, deviceBytes.size());
    EXPECT_EQ(256, blackboxHeaderBudget);
    EXPECT_EQ(0u, blackboxParamsSeq());
}

TEST(BlackboxParamsTest, ChangesHasMetaLinesOnly)
{
    setupConfig(BLACKBOX_PARAMS_CHANGES);
    const HeaderRun run = writeHeader();
    const Decoded d = decodeHeader(run.text);
    for (const std::string &e : d.errors) {
        ADD_FAILURE() << e;
    }
    const std::vector<std::string> expected = {
        "Param log", "param_mode", "param_loop", "param_phase", "param_pgs", "param_fixes",
        "param_rt", "param_pid_profile", "param_rate_profile", "param_end",
    };
    // param_pgs.1, param_pgs.2, ... continue param_pgs
    std::vector<std::string> keys;
    int continued = 0;
    for (const std::string &key : d.keyOrder) {
        if (key == "param_pgs." + std::to_string(continued + 1)) {
            continued++;
        } else {
            keys.push_back(key);
        }
    }
    EXPECT_EQ(expected, keys);
    EXPECT_GE(continued, 1);
    EXPECT_EQ("1", d.keys.at("Param log"));
    EXPECT_EQ("CHANGES", d.keys.at("param_mode"));
    EXPECT_EQ("2,2,8,128", d.keys.at("param_loop"));
    EXPECT_EQ("pos=0,sp=0,pid=0,mix=1,mot=1,fupd=1,bb=1,flush=0", d.keys.at("param_phase"));
    EXPECT_EQ("-", d.keys.at("param_fixes"));
    EXPECT_EQ("0", d.keys.at("param_pid_profile"));
    EXPECT_TRUE(d.endSeen);
    EXPECT_EQ((unsigned)d.lines, d.endLines);
    EXPECT_EQ(d.hash, d.endHash);
    printf("CHANGES header: %zu bytes, %d lines, %d calls\n", run.text.size(), d.lines + 1, run.calls);
}

TEST(BlackboxParamsTest, ParamPgsListsTheTrackedGroups)
{
    setupConfig(BLACKBOX_PARAMS_CHANGES);
    const Decoded d = decodeHeader(writeHeader().text);
    std::string expected, actual = d.keys.at("param_pgs");
    for (int k = 1; d.keys.count("param_pgs." + std::to_string(k)); k++) {
        actual += "," + d.keys.at("param_pgs." + std::to_string(k));
    }
    for (int t = 0; t < BBP_TRACKED; t++) {
        const pgRegistry_t *reg = trackedReg(t);
        if (reg) {
            expected += (expected.empty() ? "" : ",") + std::to_string(pgN(reg)) + "." + std::to_string(pgVersion(reg)) + "/" + std::to_string(pgSize(reg));
        }
    }
    EXPECT_EQ(expected, actual);
}

TEST(BlackboxParamsTest, DefaultConfigFullIsExact)
{
    setupConfig(BLACKBOX_PARAMS_FULL);
    const HeaderRun run = writeHeader();
    const Decoded d = decodeHeader(run.text);
    expectExact(d);

    // No boot lines: nothing changed since boot
    EXPECT_EQ(0, countKeys(d, "boot."));
    // The profiles of the defaults are the same: all fields after the first are empty
    for (const std::string &key : d.keyOrder) {
        if (key.compare(0, 6, "set@p.") == 0 || key.compare(0, 6, "set@r.") == 0) {
            const std::vector<std::string> fields = split(d.keys.at(key), '|');
            EXPECT_EQ(std::vector<std::string>(fields.size() - 1, ""), std::vector<std::string>(fields.begin() + 1, fields.end())) << key;
        }
    }

    int tracked = 0, bytes = 0;
    for (int t = 0; t < BBP_TRACKED; t++) {
        if (trackedReg(t)) {
            tracked++;
            bytes += pgSize(trackedReg(t));
        } else {
            printf("  group %d is not in this build\n", bbpTrackedPgs[t].pgn);
        }
    }
    if (getenv("BBP_ENTRIES_DUMP")) {
        FILE *f = fopen(getenv("BBP_ENTRIES_DUMP"), "w");
        for (int i = 0; i < valueTableEntryCount; i++) {
            if (bbpTrackedIndex(valueTable[i].pgn) >= 0) {
                fprintf(f, "%s\n", valueTable[i].name);
            }
        }
        fclose(f);
    }
    printf("FULL header, default configuration: %zu bytes, %d lines, %d calls at 64 B per call\n",
        run.text.size(), d.lines + 1, run.calls);
    printf("  %d tracked groups (%d bytes): %d set, %d set@p, %d set@r, %d el, %d pg lines\n", tracked, bytes,
        countKeys(d, "set."), countKeys(d, "set@p"), countKeys(d, "set@r"), countKeys(d, "el."), countKeys(d, "pg."));
    if (getenv("BBP_HEADER_DUMP")) {
        FILE *f = fopen(getenv("BBP_HEADER_DUMP"), "w");
        fputs(run.text.c_str(), f);
        fclose(f);
    }
}

// The time of the slowest call on this computer: the shortest of 50 runs for each call
TEST(BlackboxParamsTest, CallTime)
{
    std::vector<double> best;
    for (int run = 0; run < 50; run++) {
        setupConfig(BLACKBOX_PARAMS_FULL);
        blackboxParamsStart();
        deviceBytes.clear();
        for (int call = 0; ; call++) {
            blackboxHeaderBudget = 256;
            const auto start = std::chrono::steady_clock::now();
            const bool done = blackboxParamsWriteHeader();
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            if ((int)best.size() <= call) {
                best.push_back(us);
            }
            best[call] = std::min(best[call], us);
            if (done) {
                break;
            }
        }
    }
    double worst = 0, total = 0;
    size_t worstCall = 0;
    for (size_t call = 0; call < best.size(); call++) {
        if (best[call] > worst) {
            worst = best[call];
            worstCall = call;
        }
        total += best[call];
    }
    std::vector<double> sorted = best;
    std::sort(sorted.begin(), sorted.end());
    printf("call time on this computer: slowest %.2f us (call %zu), 99th percentile %.2f us, mean %.3f us, %zu calls\n",
        worst, worstCall, sorted[sorted.size() * 99 / 100], total / best.size(), best.size());
    if (getenv("BBP_CALL_TIMES")) {
        for (size_t call = 0; call < best.size(); call++) {
            printf("%zu %.3f\n", call, best[call]);
        }
    }
}

TEST(BlackboxParamsTest, ElementRunsAndProfileFields)
{
    setupConfig(BLACKBOX_PARAMS_FULL);
    // Servos 0-3 the same, 4 different, 5 and up the same as 0-3
    for (int i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        servoParamsMutable(i)->mid = 1500;
    }
    servoParamsMutable(4)->mid = 1520;
    pidProfilesMutable(2)->pid[0].P = 77;
    const Decoded d = decodeHeader(writeHeader().text);
    expectExact(d);
    EXPECT_EQ(1u, d.keys.count("el.servo.0-3"));
    EXPECT_EQ(1u, d.keys.count("el.servo.4"));
    EXPECT_EQ(1u, d.keys.count("el.servo.5-" + std::to_string(MAX_SUPPORTED_SERVOS - 1)));
    EXPECT_EQ(0u, d.keys.at("el.servo.4").find("1520,"));
    const std::string p = std::to_string(pidProfiles(0)->pid[0].P);
    std::string expected = p + "||77";
    for (int k = 3; k < PID_PROFILE_COUNT; k++) {
        expected += (k == 3) ? "|" + p : "|";
    }
    EXPECT_EQ(expected, d.keys.at("set@p.roll_p_gain"));
}

TEST(BlackboxParamsTest, RandomConfigFullIsExact)
{
    const int iterations = getenv("BBP_RANDOM_ITERATIONS") ? atoi(getenv("BBP_RANDOM_ITERATIONS")) : 40;
    for (int n = 0; n < iterations; n++) {
        setupConfig(BLACKBOX_PARAMS_FULL);
        for (int t = 0; t < BBP_TRACKED; t++) {
            if (trackedReg(t)) {
                fillRandom(trackedReg(t)->address, pgSize(trackedReg(t)));
            }
        }
        tidyStrings();
        blackboxConfigMutable()->params = BLACKBOX_PARAMS_FULL;
        if (n % 2) {
            // Profiles that repeat
            for (int k = 1; k < PID_PROFILE_COUNT; k += 2) {
                *pidProfilesMutable(k) = *pidProfiles(k - 1);
            }
        }
        currentPidProfile = pidProfilesMutable(n % PID_PROFILE_COUNT);
        const HeaderRun run = writeHeader(n % 3 ? 64 : 40);
        const Decoded d = decodeHeader(run.text);
        expectExact(d);
        EXPECT_EQ(std::to_string(n % PID_PROFILE_COUNT), d.keys.at("param_pid_profile"));
        // The values are the boot values: boot lines only where a group differs from the boot copy
        EXPECT_GT(countKeys(d, "boot."), 0);
        if (HasFailure()) {
            printf("%s", run.text.c_str());
            break;
        }
    }
}

TEST(BlackboxParamsTest, BootLines)
{
    setupConfig(BLACKBOX_PARAMS_FULL);
    const uint8_t mode = governorConfig()->gov_mode;
    const uint16_t mid = servoParams(1)->mid;
    const char board0 = systemConfig()->boardIdentifier[0];

    // Changes after boot in boot-cached groups
    governorConfigMutable()->gov_mode = mode + 1;
    servoParamsMutable(1)->mid = mid + 10;
    systemConfigMutable()->boardIdentifier[0] = board0 + 1;
    // Not a boot value: the profile index
    systemConfigMutable()->pidProfileIndex = 3;
    // Not a boot-cached group
    pidProfilesMutable(0)->pid[0].P += 1;

    const Decoded d = decodeHeader(writeHeader().text);
    expectExact(d);

    const clivalue_t *v = findEntry("gov_mode", MASTER_VALUE);
    ASSERT_NE(nullptr, v);
    char text[64];
    bbpWriter_t w = { text, text + sizeof(text) - 1, false };
    uint8_t boot = mode;
    bbpPutValue(&w, v, &boot);
    *w.ptr = 0;
    EXPECT_EQ(text, d.keys.at("boot.set.gov_mode"));

    EXPECT_EQ(0u, d.keys.at("boot.el.servo.1").find(std::to_string(mid) + ","));
    EXPECT_EQ(1u, d.keys.count("boot.pg.18+0"));
    EXPECT_EQ(3, countKeys(d, "boot."));

    // A profile switch alone gives no boot line
    setupConfig(BLACKBOX_PARAMS_FULL);
    systemConfigMutable()->pidProfileIndex = 2;
    systemConfigMutable()->activeRateProfile = 1;
    const Decoded d2 = decodeHeader(writeHeader().text);
    expectExact(d2);
    EXPECT_EQ(0, countKeys(d2, "boot."));
}

TEST(BlackboxParamsTest, LongLinesFallBack)
{
    // set@p lines that are too long become one line per profile
    setupConfig(BLACKBOX_PARAMS_FULL);
    for (int k = 0; k < PID_PROFILE_COUNT; k++) {
        // 22 chars of text for each profile
        memset(pidProfilesMutable(k)->profileName, '%', MAX_PROFILE_NAME_LENGTH);
        pidProfilesMutable(k)->profileName[MAX_PROFILE_NAME_LENGTH - 1] = 'a' + k;
    }
    const Decoded d = decodeHeader(writeHeader().text);
    expectExact(d);
#if BBP_LINE_MAX < 192
    EXPECT_EQ(1u, d.keys.count("set@p0.profile_name"));
    EXPECT_EQ(0u, d.keys.count("set@p.profile_name"));
#else
    EXPECT_EQ(1u, d.keys.count("set@p.profile_name"));
#endif
}
