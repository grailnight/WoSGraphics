#pragma once
// Sub-pixel jitter for native vertex shaders, by bytecode rewriting.
//
// The engine's shaders are never modified in place: other passes identify them
// by their exact bytes (cascade probe, RSM capture). A twin is built instead and
// bound only for the duration of a main-view draw.
//
// Every write to the position output is redirected to a free temporary T, and
// just before the main function returns two instructions are appended:
//
//     mul  U, cJ, T.wwww      ; U = (jx*w, jy*w, 0, 0)
//     add  oPos, T, U
//
// cJ is a float constant register no part of the shader can reach, holding the
// jitter in clip units with zw = 0. With cJ = 0 the added term is +0, so the
// position, and with it rasterisation and depth, is unchanged bit for bit.
//
// Supported: vs_2_0, vs_2_x, vs_3_0. Anything the rewrite cannot prove safe
// (vs_1_x, relative writes to outputs, relative constant reads without a CTAB,
// no free temporaries, no free constant) is rejected and drawn unjittered.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace WosTaaPatch {

enum : uint32_t {
    kOpNop = 0, kOpMov = 1, kOpAdd = 2, kOpMul = 5, kOpCall = 25, kOpCallNz = 26,
    kOpRet = 28, kOpLabel = 30, kOpDcl = 31, kOpDef = 81, kOpDefB = 47, kOpDefI = 48,
    kOpComment = 0xFFFE, kOpEnd = 0xFFFF,
};
enum : uint32_t {
    kRegTemp = 0, kRegConst = 2, kRegRastOut = 4, kRegOutput = 6, kRegConst2 = 11,
    kRegConst3 = 12, kRegConst4 = 13,
};

inline uint32_t RegType(uint32_t t) { return ((t >> 28) & 7) | ((t & 0x1800) >> 8); }
inline uint32_t RegNum(uint32_t t) { return t & 0x7FF; }
inline uint32_t WithReg(uint32_t t, uint32_t type, uint32_t num) {
    t &= ~(0x70000000u | 0x1800u | 0x7FFu);
    return t | ((type << 28) & 0x70000000u) | ((type << 8) & 0x1800u) | (num & 0x7FF);
}
inline uint32_t Dst(uint32_t type, uint32_t num, uint32_t mask) {
    return WithReg(0x80000000u | (mask << 16), type, num);
}
inline uint32_t Src(uint32_t type, uint32_t num, uint32_t swizzle) {
    return WithReg(0x80000000u | (swizzle << 16), type, num);
}
static const uint32_t kSwizzleXYZW = 0xE4, kSwizzleWWWW = 0xFF;
static const uint32_t kRelative = 0x2000; // D3DSHADER_ADDRMODE_RELATIVE

struct Result {
    std::vector<DWORD> code;
    unsigned jitterRegister = 0, positionTemp = 0, scratchTemp = 0;
    unsigned redirectedWrites = 0;
    size_t insertAt = 0; // word index of the appended block in code
    bool subroutines = false;
};

// Float constant ranges declared in the CTAB, as [first, end).
static void CtabFloatRanges(const DWORD* comment, size_t words, std::vector<std::pair<unsigned, unsigned>>& out, bool& found) {
    if (words < 1 || comment[0] != MAKEFOURCC('C', 'T', 'A', 'B')) return;
    const size_t bytes = (words - 1) * 4;
    const unsigned char* base = reinterpret_cast<const unsigned char*>(comment + 1);
    auto u32 = [&](size_t at, uint32_t& v) { if (at + 4 > bytes) return false; std::memcpy(&v, base + at, 4); return true; };
    uint32_t count = 0, info = 0;
    if (!u32(12, count) || !u32(16, info) || count > 4096) return;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = size_t(info) + size_t(i) * 20;
        if (at + 20 > bytes) return;
        uint16_t set = 0, index = 0, n = 0;
        std::memcpy(&set, base + at + 4, 2); std::memcpy(&index, base + at + 6, 2); std::memcpy(&n, base + at + 8, 2);
        if (set == 2) out.push_back({ index, unsigned(index) + n });
    }
    found = true;
}

inline bool Patch(const DWORD* input, size_t words, unsigned maxConstants, Result& result, std::string& why) {
    result = Result{};
    if (!input || words < 2) { why = "empty"; return false; }
    const uint32_t version = input[0];
    if ((version & 0xFFFF0000u) != 0xFFFE0000u) { why = "not a vertex shader"; return false; }
    const unsigned major = (version >> 8) & 0xFF;
    if (major < 2 || major > 3) { why = "unsupported shader model"; return false; }

    struct Instruction { size_t at, length; uint32_t opcode; };
    std::vector<Instruction> program;
    bool tempUsed[32] = {};
    std::vector<bool> constUsed(maxConstants, false);
    std::vector<std::pair<unsigned, unsigned>> ctab;
    bool haveCtab = false, relativeConst = false, ended = false;
    int positionOutput = -1; // SM3: output register declared as POSITION0
    unsigned temporaries = 12;

    for (size_t at = 1; at < words && !ended;) {
        const uint32_t token = input[at], opcode = token & 0xFFFF;
        if (opcode == kOpComment) {
            const size_t length = (token >> 16) & 0x7FFF;
            if (at + 1 + length > words) { why = "truncated comment"; return false; }
            CtabFloatRanges(input + at + 1, length, ctab, haveCtab);
            program.push_back({ at, length, opcode });
            at += 1 + length;
            continue;
        }
        if (opcode == kOpEnd) { program.push_back({ at, 0, opcode }); ended = true; ++at; break; }
        const size_t length = (token >> 24) & 0x0F;
        if (at + 1 + length > words) { why = "truncated instruction"; return false; }
        program.push_back({ at, length, opcode });
        const size_t first = at + 1, last = at + 1 + length;
        if (opcode == kOpDef || opcode == kOpDefI || opcode == kOpDefB) {
            if (length < 1) { why = "bad def"; return false; }
            const uint32_t reg = input[first];
            if (RegType(reg) == kRegConst && RegNum(reg) < maxConstants) constUsed[RegNum(reg)] = true;
            at = last; continue;
        }
        size_t p = first;
        if (opcode == kOpDcl) {
            if (length < 2) { why = "bad dcl"; return false; }
            const uint32_t usage = input[p] & 0x1F, usageIndex = (input[p] >> 16) & 0xF;
            const uint32_t reg = input[p + 1];
            if (major == 3 && RegType(reg) == kRegOutput && usage == 0 && usageIndex == 0) {
                if (positionOutput >= 0) { why = "two position outputs"; return false; }
                positionOutput = int(RegNum(reg));
            }
            at = last; continue;
        }
        if (opcode == kOpCall || opcode == kOpCallNz || opcode == kOpLabel) result.subroutines = true;
        for (; p < last; ++p) {
            const uint32_t reg = input[p];
            const uint32_t type = RegType(reg), num = RegNum(reg);
            if (type == kRegTemp && num < 32) tempUsed[num] = true;
            if (type == kRegConst) {
                if (num < maxConstants) constUsed[num] = true;
                if (reg & kRelative) relativeConst = true;
            }
            if (type == kRegConst2 || type == kRegConst3 || type == kRegConst4) { why = "constant above 2047"; return false; }
            if (type == kRegOutput && (reg & kRelative)) { why = "relative output write"; return false; }
            // A relative parameter carries one extra address token in SM2+.
            if ((reg & kRelative) && major >= 2) ++p;
        }
        at = last;
    }
    if (!ended) { why = "no end token"; return false; }
    if (relativeConst && !haveCtab) { why = "relative constants without CTAB"; return false; }

    const uint32_t posType = major == 3 ? kRegOutput : kRegRastOut;
    const uint32_t posNum = major == 3 ? uint32_t(positionOutput) : 0;
    if (major == 3 && positionOutput < 0) { why = "no position output"; return false; }

    // Two temporaries nobody touches.
    int t = -1, u = -1;
    for (unsigned i = 0; i < temporaries; ++i) if (!tempUsed[i]) { if (t < 0) t = int(i); else if (u < 0) u = int(i); }
    if (t < 0 || u < 0) { why = "no free temporaries"; return false; }

    // The highest float constant above everything the shader or its CTAB names.
    unsigned floor = 0;
    for (unsigned i = 0; i < maxConstants; ++i) if (constUsed[i]) floor = i + 1;
    for (auto& r : ctab) if (r.second > floor) floor = r.second;
    if (maxConstants == 0 || floor >= maxConstants) { why = "no free constant"; return false; }
    const unsigned jitter = maxConstants - 1;

    // Main ends at the first ret preceding the first label, else at END.
    size_t insertAt = 0;
    {
        size_t firstLabel = program.size();
        for (size_t i = 0; i < program.size(); ++i) if (program[i].opcode == kOpLabel) { firstLabel = i; break; }
        size_t index = program.size();
        if (firstLabel < program.size()) {
            for (size_t i = 0; i < firstLabel; ++i) if (program[i].opcode == kOpRet) { index = i; break; }
            if (index == program.size()) { why = "subroutines without a main ret"; return false; }
        } else {
            for (size_t i = 0; i < program.size(); ++i) if (program[i].opcode == kOpEnd) { index = i; break; }
        }
        insertAt = program[index].at;
    }

    std::vector<DWORD> out(input, input + words);
    unsigned covered = 0;
    for (const auto& ins : program) {
        if (ins.opcode == kOpComment || ins.opcode == kOpEnd || ins.opcode == kOpDcl ||
            ins.opcode == kOpDef || ins.opcode == kOpDefI || ins.opcode == kOpDefB || ins.length == 0) continue;
        const uint32_t dst = input[ins.at + 1];
        if (!(dst & 0x80000000u)) continue;
        if (RegType(dst) == posType && RegNum(dst) == posNum) {
            out[ins.at + 1] = WithReg(dst, kRegTemp, uint32_t(t));
            covered |= (dst >> 16) & 0xF;
            ++result.redirectedWrites;
        }
    }
    if (result.redirectedWrites == 0) { why = "position never written"; return false; }
    // T.w is read below; a shader that never writes all of it would read garbage.
    if (covered != 0xF) { why = "position not fully written"; return false; }

    const DWORD tail[] = {
        kOpMul | (3u << 24), Dst(kRegTemp, u, 0xF), Src(kRegConst, jitter, kSwizzleXYZW), Src(kRegTemp, t, kSwizzleWWWW),
        kOpAdd | (3u << 24), Dst(posType, posNum, 0xF), Src(kRegTemp, t, kSwizzleXYZW), Src(kRegTemp, u, kSwizzleXYZW),
    };
    out.insert(out.begin() + insertAt, tail, tail + sizeof(tail) / sizeof(tail[0]));
    result.code.swap(out);
    result.insertAt = insertAt;
    result.jitterRegister = jitter;
    result.positionTemp = unsigned(t);
    result.scratchTemp = unsigned(u);
    return true;
}

} // namespace WosTaaPatch
