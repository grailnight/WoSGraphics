#pragma once
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Append zero auxiliary coverage without disassembly, register renaming, or any
// changes to the native instruction words. Unsupported bytecode fails closed.
namespace WosIndirectGuard {
static bool Build(const std::vector<unsigned char>& input, std::vector<unsigned char>& output,
                  std::string& reason, unsigned* zeroRegister = nullptr) {
    output.clear();
    auto reject = [&](const char* why) { reason = why; return false; };
    constexpr size_t maxBytes = 1024 * 1024;
    if (input.size() < 8 || input.size() > maxBytes - 48 || input.size() % 4)
        return reject("invalid shader byte length");
    std::vector<uint32_t> words(input.size() / 4);
    std::memcpy(words.data(), input.data(), input.size());
    if (words[0] != 0xffff0300u) return reject("only ps_3_0 runtime guards are supported");
    bool constants[224] = {};
    std::vector<unsigned> flow;
    size_t insertion = 1, end = 0;
    bool leading = true;
    auto operand = [&](uint32_t token) {
        if (!(token & 0x80000000u)) { reason = "invalid operand token"; return false; }
        if (token & 0x00002000u) { reason = "relative addressing is unsupported"; return false; }
        unsigned type = ((token >> 28) & 7) | ((token >> 8) & 0x18);
        unsigned reg = token & 0x7ff;
        if (type == D3DSPR_COLOROUT && reg != 0) { reason = "shader already writes auxiliary color outputs"; return false; }
        if (type == D3DSPR_CONST) {
            if (reg >= 224) { reason = "pixel float constant index out of bounds"; return false; }
            constants[reg] = true;
        }
        if (type == D3DSPR_CONST2 || type == D3DSPR_CONST3 || type == D3DSPR_CONST4 ||
            type == D3DSPR_LABEL || type > D3DSPR_PREDICATE) { reason = "unsupported register file"; return false; }
        return true;
    };
    for (size_t at = 1; at < words.size();) {
        uint32_t token = words[at];
        unsigned op = token & D3DSI_OPCODE_MASK;
        if (op == D3DSIO_COMMENT) {
            if (token & 0x80000000u) return reject("invalid comment token");
            size_t count = (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
            if (count > words.size() - at - 1) return reject("truncated comment");
            at += count + 1;
            if (leading) insertion = at;
            continue;
        }
        leading = false;
        if (op == D3DSIO_END) {
            if (token != D3DSIO_END || at != words.size() - 1 || !flow.empty())
                return reject("invalid END, trailing words, or unclosed flow");
            end = at;
            break;
        }
        if (token & 0xf0000000u) return reject("predicated or reserved instruction bits are unsupported");
        if (op == D3DSIO_CALL || op == D3DSIO_CALLNZ || op == D3DSIO_RET || op == D3DSIO_LABEL)
            return reject("subroutines or early return are unsupported");
        unsigned length = (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (length > words.size() - at - 1) return reject("truncated instruction");
        unsigned expected = 99;
        switch (op) {
        case D3DSIO_NOP: case D3DSIO_ENDLOOP: case D3DSIO_ENDREP:
        case D3DSIO_ELSE: case D3DSIO_ENDIF: case D3DSIO_BREAK: expected = 0; break;
        case D3DSIO_TEXKILL: case D3DSIO_REP: case D3DSIO_IF: case D3DSIO_BREAKP: expected = 1; break;
        case D3DSIO_MOV: case D3DSIO_RCP: case D3DSIO_RSQ: case D3DSIO_EXP: case D3DSIO_LOG:
        case D3DSIO_FRC: case D3DSIO_ABS: case D3DSIO_NRM: case D3DSIO_SINCOS:
        case D3DSIO_DSX: case D3DSIO_DSY: case D3DSIO_LOOP: case D3DSIO_IFC: case D3DSIO_BREAKC:
        case D3DSIO_DCL: case D3DSIO_DEFB: expected = 2; break;
        case D3DSIO_ADD: case D3DSIO_SUB: case D3DSIO_MUL: case D3DSIO_DP3: case D3DSIO_DP4:
        case D3DSIO_MIN: case D3DSIO_MAX: case D3DSIO_SLT: case D3DSIO_SGE:
        case D3DSIO_POW: case D3DSIO_CRS: case D3DSIO_TEX: case D3DSIO_TEXLDL:
        case D3DSIO_SETP: expected = 3; break;
        case D3DSIO_MAD: case D3DSIO_LRP: case D3DSIO_CMP: case D3DSIO_DP2ADD: expected = 4; break;
        case D3DSIO_DEF: case D3DSIO_DEFI: case D3DSIO_TEXLDD: expected = 5; break;
        // Matrix opcodes implicitly read adjacent constants. Rejecting them
        // avoids falsely selecting a constant used without an operand token.
        default: return reject("unsupported opcode (including implicit matrix registers)");
        }
        if (length != expected) return reject("instruction operand count mismatch");
        if (op == D3DSIO_DEF || op == D3DSIO_DEFI || op == D3DSIO_DEFB) {
            unsigned type = ((words[at+1] >> 28) & 7) | ((words[at+1] >> 8) & 0x18);
            unsigned wanted = op == D3DSIO_DEF ? D3DSPR_CONST : op == D3DSIO_DEFI ? D3DSPR_CONSTINT : D3DSPR_CONSTBOOL;
            if (type != wanted || !operand(words[at+1])) return reject("invalid constant definition destination");
            // The remaining words are float/int/bool literals, not operands.
        } else if (op == D3DSIO_DCL) {
            if (!(words[at+1] & 0x80000000u) || !operand(words[at+2])) return reject("invalid declaration");
            // The first DCL word is a semantic/texture-type token.
        } else {
            for (unsigned i=1; i<=length; ++i) if (!operand(words[at+i])) return false;
        }
        if (op == D3DSIO_IF || op == D3DSIO_IFC) flow.push_back(D3DSIO_IF);
        else if (op == D3DSIO_LOOP || op == D3DSIO_REP) flow.push_back(op);
        else if (op == D3DSIO_ELSE) {
            if (flow.empty() || flow.back() != D3DSIO_IF) return reject("unmatched or duplicate ELSE");
            flow.back() = D3DSIO_ELSE;
        } else if (op == D3DSIO_ENDIF || op == D3DSIO_ENDLOOP || op == D3DSIO_ENDREP) {
            unsigned wanted = op == D3DSIO_ENDLOOP ? D3DSIO_LOOP : op == D3DSIO_ENDREP ? D3DSIO_REP : D3DSIO_IF;
            if (flow.empty() || (flow.back() != wanted && !(wanted == D3DSIO_IF && flow.back() == D3DSIO_ELSE)))
                return reject("unmatched flow terminator");
            flow.pop_back();
        } else if (op == D3DSIO_BREAK || op == D3DSIO_BREAKC || op == D3DSIO_BREAKP) {
            bool loop = false;
            for (unsigned item : flow) if (item == D3DSIO_LOOP || item == D3DSIO_REP) loop = true;
            if (!loop) return reject("BREAK outside loop");
        }
        at += length + 1;
    }
    if (!end) return reject("missing END token");
    unsigned constant = 224;
    while (constant && constants[constant-1]) --constant;
    if (!constant) return reject("no free float constant");
    --constant;
    std::vector<uint32_t> generated;
    generated.reserve(words.size() + 12);
    generated.insert(generated.end(), words.begin(), words.begin() + insertion);
    const uint32_t definition[] = {0x05000051u, 0xa00f0000u | constant, 0, 0, 0, 0};
    generated.insert(generated.end(), definition, definition + 6);
    generated.insert(generated.end(), words.begin() + insertion, words.begin() + end);
    const uint32_t guards[] = {0x02000001u, 0x800f0801u, 0xa0e40000u | constant,
                              0x02000001u, 0x800f0802u, 0xa0e40000u | constant};
    generated.insert(generated.end(), guards, guards + 6);
    generated.push_back(words[end]);
    output.resize(generated.size() * 4);
    std::memcpy(output.data(), generated.data(), output.size());
    if (zeroRegister) *zeroRegister = constant;
    reason = "runtime zero-coverage guard generated";
    return true;
}
}
