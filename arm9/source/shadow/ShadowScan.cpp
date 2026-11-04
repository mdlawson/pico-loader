#include "ShadowScan.h"

namespace shadow_scan
{

static inline u32 ror(u32 v, u32 r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

const Block* Scanner::BlockOf(u32 a) const
{
    for (u32 i = 0; i < _blockCount; i++)
        if (a - _blocks[i].runAddress < _blocks[i].size)
            return &_blocks[i];
    return nullptr;
}

bool Scanner::Read32(u32 a, u32& v) const
{
    if (a & 3)
        return false;
    const Block* b = BlockOf(a);
    if (!b || a - b->runAddress + 4 > b->size)
        return false;
    const uint8_t* p = b->data + (a - b->runAddress);
    v = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
    return true;
}

bool Scanner::Read16(u32 a, u32& v) const
{
    if (a & 1)
        return false;
    const Block* b = BlockOf(a);
    if (!b || a - b->runAddress + 2 > b->size)
        return false;
    const uint8_t* p = b->data + (a - b->runAddress);
    v = p[0] | (p[1] << 8);
    return true;
}

// A store just before a backward branch is in a loop: a copy or fill routine
// that got an io address, which is better handled where it's called.
bool Scanner::InLoop(u32 site, bool thumb) const
{
    for (u32 i = 1; i <= 8; i++)
    {
        u32 op;
        if (thumb)
        {
            u32 pc = site + 2 * i;
            if (!Read16(pc, op)) return false;
            if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00)
                return pc + 4 + ((int32_t)(op << 24) >> 23) <= site;
            if ((op & 0xF800) == 0xE000)
                return pc + 4 + ((int32_t)(op << 21) >> 20) <= site;
        }
        else
        {
            u32 pc = site + 4 * i;
            if (!Read32(pc, op)) return false;
            if ((op & 0x0F000000) == 0x0A000000 && (op >> 28) != 0xF)
                return pc + 8 + ((u32)((int32_t)(op << 8) >> 6)) <= site;
        }
    }
    return false;
}

void Scanner::AddSiteAt(u32 site, bool thumb, int depth)
{
    if (depth > 0 && InLoop(site, thumb))
        return;
    AddSite(site | (thumb ? 1 : 0));
}

void Scanner::Call(u32 target, bool thumb, const Regs& r, int depth)
{
    bool args = false;
    for (int i = 0; i < 4; i++)
        if (r.kind[i] == IO) args = true;
    if (!args || depth >= 2 || !BlockOf(target))
        return;
    u32 key = thumb;
    for (int i = 0; i < 4; i++)
        if (r.kind[i] == IO) key = key * 31 + r.val[i] + i;
    for (u32 i = 0; i < _callKeyN && i < 64; i++)
        if (_callKey[i][0] == target && _callKey[i][1] == key)
            return;
    _callKey[_callKeyN & 63][0] = target;
    _callKey[_callKeyN & 63][1] = key;
    _callKeyN++;
    Regs c;
    for (int i = 0; i < 16; i++) c.kind[i] = NONE;
    for (int i = 0; i < 4; i++) { c.kind[i] = r.kind[i]; c.val[i] = r.val[i]; }
    if (thumb) TraceThumb(target & ~1u, c, depth + 1);
    else Trace(target, c, depth + 1);
}

void Scanner::AddSite(u32 site)
{
    for (u32 i = 0; i < _siteCount; i++)
        if (_sites[i] == site)
            return;
    if (_siteCount < _maxSites)
        _sites[_siteCount++] = site;
    else
        _overflow++;
}

bool Scanner::AnyIo(const Regs& r) const
{
    for (int i = 0; i < 13; i++)
        if (r.kind[i] == IO)
            return true;
    return false;
}

static inline bool isIo(u32 v) { return v >= 0x04000000 && v < 0x04002000; }

// does a store of size bytes at a touch a shadowed register?
static bool touches(u32 a, u32 size)
{
    for (u32 k = 0; k < size; k++)
        if (IsShadowed(a + k))
            return true;
    return false;
}

void Scanner::Trace(u32 pc, Regs r, int depth)
{
    for (int n = 0; n < 256; n++, pc += 4)
    {
        u32 op;
        if (!Read32(pc, op))
            return;
        u32 cond = op >> 28;
        bool al = cond == 0xE;
        if (cond == 0xF)
        {
            // blx imm: a call into thumb code
            if ((op & 0x0E000000) == 0x0A000000)
            {
                Call(pc + 8 + ((u32)((int32_t)(op << 8) >> 6)) + ((op >> 23) & 2), true, r, depth);
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            continue; // pld and the like
        }
        u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
        auto kill = [&](u32 x) { if (x < 16) r.kind[x] = NONE; };
        auto setv = [&](u32 x, u32 v) { if (x < 13) { r.kind[x] = isIo(v) ? IO : CONST; r.val[x] = v; } };
        u32 t = (op >> 25) & 7;

        // branches
        if (t == 5)
        {
            if (op & 0x01000000)
            {
                // bl: follow the callee with the io arguments
                Call(pc + 8 + ((u32)((int32_t)(op << 8) >> 6)), false, r, depth);
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            if (al)
                return;
            continue; // a conditional branch: on along the fall-through
        }
        // bx / blx register
        if ((op & 0x0FFFFFD0) == 0x012FFF10)
        {
            if (op & 0x20)
            {
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            if (al)
                return;
            continue;
        }
        // block transfer
        if (t == 4)
        {
            u32 list = op & 0xFFFF;
            bool load = op & (1 << 20), wb = op & (1 << 21), up = op & (1 << 23), pre = op & (1 << 24);
            if (load)
            {
                if ((list & 0x8000) && al)
                    return;
                for (int i = 0; i < 16; i++)
                    if (list & (1 << i)) kill(i);
                if (wb) kill(rn);
            }
            else if (rn < 13 && r.kind[rn] == IO)
            {
                u32 cnt = 0;
                for (int i = 0; i < 16; i++)
                    if (list & (1 << i)) cnt++;
                u32 b = r.val[rn];
                u32 lo = up ? b + (pre ? 4 : 0) : b - 4 * cnt + (pre ? 0 : 4);
                if (touches(lo, 4 * cnt) && !(list & 0xE000))
                    AddSiteAt(pc, false, depth);
                if (wb)
                {
                    if (al) r.val[rn] = up ? b + 4 * cnt : b - 4 * cnt;
                    else kill(rn);
                }
            }
            else if (wb)
                kill(rn);
            if (!AnyIo(r))
                return;
            continue;
        }
        // single transfer: ldr/str/ldrb/strb
        if (t == 2 || t == 3)
        {
            if (t == 3 && (op & 0x10))
            {
                kill(rd); kill(rn); // media/undefined
                if (!AnyIo(r)) return;
                continue;
            }
            bool load = op & (1 << 20), wb = op & (1 << 21), byte = op & (1 << 22), up = op & (1 << 23), pre = op & (1 << 24);
            bool imm = t == 2;
            u32 off = op & 0xFFF;
            if (load)
            {
                if (rd == 15 && al)
                    return;
                if (rn == 15 && imm && pre && !wb && !byte && al)
                {
                    u32 v;
                    if (Read32(pc + 8 + (up ? off : -off), v)) setv(rd, v);
                    else kill(rd);
                }
                else
                    kill(rd);
                if ((wb || !pre) && rn != rd) kill(rn);
            }
            else if (rn < 13 && r.kind[rn] == IO)
            {
                u32 b = r.val[rn];
                if (imm)
                {
                    u32 a = pre ? (up ? b + off : b - off) : b;
                    if (rd < 13 && touches(a, byte ? 1 : 4))
                        AddSiteAt(pc, false, depth);
                    if (wb || !pre)
                    {
                        if (al) r.val[rn] = up ? b + off : b - off;
                        else kill(rn);
                    }
                }
                else if (wb || !pre)
                    kill(rn);
            }
            else if ((wb || !pre) && rn < 16)
                kill(rn);
            if (!AnyIo(r))
                return;
            continue;
        }
        // halfword / doubleword transfers, multiplies, swaps (bits 27-25 = 000, bit 7 and bit 4 set)
        if (t == 0 && (op & 0x90) == 0x90)
        {
            u32 sh = (op >> 5) & 3;
            if (sh == 0)
            {
                // multiplies and swaps: rd at 19-16 (and 15-12 for the long ones)
                kill(rn); kill(rd);
                if (!AnyIo(r)) return;
                continue;
            }
            bool load = op & (1 << 20), wb = op & (1 << 21), immForm = op & (1 << 22), up = op & (1 << 23), pre = op & (1 << 24);
            u32 off = ((op >> 4) & 0xF0) | (op & 0xF);
            bool store = !load && (sh == 1 || sh == 3); // strh, strd
            if (store && rn < 13 && r.kind[rn] == IO)
            {
                u32 b = r.val[rn];
                if (immForm)
                {
                    u32 a = pre ? (up ? b + off : b - off) : b;
                    u32 size = sh == 1 ? 2 : 8;
                    bool regsOk = sh == 1 ? rd < 13 : (rd < 12 && !(rd & 1));
                    if (regsOk && touches(a, size))
                        AddSiteAt(pc, false, depth);
                    if (wb || !pre)
                    {
                        if (al) r.val[rn] = up ? b + off : b - off;
                        else kill(rn);
                    }
                }
                else if (wb || !pre)
                    kill(rn);
            }
            else
            {
                if (!store)
                {
                    if (rd == 15 && load && al) return;
                    kill(rd);
                    if (!load && sh == 2) kill(rd + 1); // ldrd
                }
                if (wb || !pre) kill(rn);
            }
            if (!AnyIo(r))
                return;
            continue;
        }
        // data processing (and the misc instructions in its space)
        if (t == 0 || t == 1)
        {
            u32 opc = (op >> 21) & 0xF;
            bool s = op & (1 << 20);
            if (opc >= 8 && opc <= 11 && !s)
            {
                // mrs, msr, bx, clz, qadd...: may write rd
                if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0FFF0FF0) == 0x016F0F10 || (op & 0x0F900FF0) == 0x01000050)
                    kill(rd);
                if (!AnyIo(r)) return;
                continue;
            }
            if (opc >= 8 && opc <= 11)
                continue; // tst, teq, cmp, cmn
            bool haveB = false;
            u32 bv = 0;
            if (t == 1)
            {
                bv = ror(op & 0xFF, ((op >> 8) & 0xF) * 2);
                haveB = true;
            }
            else if ((op & 0xFF0) == 0) // a register with no shift
            {
                u32 rm = op & 0xF;
                if (rm < 13 && r.kind[rm] != NONE) { bv = r.val[rm]; haveB = true; }
            }
            if (rd == 15)
            {
                if (al) return;
                continue;
            }
            bool haveA = rn < 13 && r.kind[rn] != NONE;
            u32 av = haveA ? r.val[rn] : 0;
            if (al && haveB && opc == 13) setv(rd, bv);
            else if (al && haveB && haveA && opc == 4) setv(rd, av + bv);
            else if (al && haveB && haveA && opc == 2) setv(rd, av - bv);
            else if (al && haveB && haveA && opc == 12) setv(rd, av | bv);
            else if (al && haveB && haveA && opc == 14) setv(rd, av & ~bv);
            else kill(rd);
            if (!AnyIo(r))
                return;
            continue;
        }
        // coprocessor, swi: mrc writes rd
        if (t == 7)
        {
            if ((op & 0x0F100010) == 0x0E100010) kill(rd);
            if ((op & 0x0F000000) == 0x0F000000)
            {
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = NONE; // swi
                if (!AnyIo(r)) return;
            }
            continue;
        }
        // t == 6: coprocessor transfers (ldc/stc): writeback only
        if (op & (1 << 21)) kill(rn);
        if (!AnyIo(r))
            return;
    }
}

void Scanner::TraceThumb(u32 pc, Regs r, int depth)
{
    auto kill = [&](u32 x) { if (x < 16) r.kind[x] = NONE; };
    auto setv = [&](u32 x, u32 v) { if (x < 13) { r.kind[x] = isIo(v) ? IO : CONST; r.val[x] = v; } };
    auto known = [&](u32 x) { return x < 13 && r.kind[x] != NONE; };
    auto store = [&](u32 rn, u32 rd, u32 off, u32 size) {
        if (rn < 8 && r.kind[rn] == IO && touches(r.val[rn] + off, size))
            AddSiteAt(pc, true, depth);
        (void)rd;
    };
    for (int n = 0; n < 256; n++, pc += 2)
    {
        u32 op;
        if (!Read16(pc, op))
            return;
        u32 rd = op & 7, rn = (op >> 3) & 7;
        switch (op >> 11)
        {
            case 0x00: case 0x01: case 0x02: // lsl, lsr, asr #imm
                if ((op >> 11) == 0 && known(rn)) setv(rd, r.val[rn] << ((op >> 6) & 31));
                else kill(rd);
                break;
            case 0x03: // add/sub register or imm3
            {
                u32 rm = (op >> 6) & 7;
                bool immf = op & 0x400, sub = op & 0x200;
                if (known(rn) && (immf || known(rm)))
                {
                    u32 b = immf ? rm : r.val[rm];
                    setv(rd, sub ? r.val[rn] - b : r.val[rn] + b);
                }
                else kill(rd);
                break;
            }
            case 0x04: setv((op >> 8) & 7, op & 0xFF); break; // movs #imm8
            case 0x05: break; // cmp #imm8
            case 0x06: case 0x07: // adds/subs rd, #imm8
            {
                u32 x = (op >> 8) & 7;
                if (known(x)) setv(x, (op >> 11) == 6 ? r.val[x] + (op & 0xFF) : r.val[x] - (op & 0xFF));
                break;
            }
            case 0x08:
                if (!(op & 0x400))
                {
                    // alu: orr keeps a known value, tst/cmp/cmn write nothing
                    u32 alu = (op >> 6) & 0xF;
                    if (alu == 8 || alu == 10 || alu == 11) break;
                    if (alu == 12 && known(rd) && known(rn)) setv(rd, r.val[rd] | r.val[rn]);
                    else kill(rd);
                    break;
                }
                else
                {
                    // hi register ops and bx/blx
                    u32 h = (op >> 8) & 3;
                    u32 rm = (op >> 3) & 0xF, d = (op & 7) | ((op >> 4) & 8);
                    if (h == 3)
                    {
                        if (op & 0x80)
                        {
                            r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = NONE;
                            break;
                        }
                        return; // bx
                    }
                    if (h == 1) break; // cmp
                    if (d == 15) return;
                    if (h == 2) { if (known(rm)) setv(d, r.val[rm]); else kill(d); }
                    else { if (known(rm) && known(d)) setv(d, r.val[d] + r.val[rm]); else kill(d); }
                    break;
                }
            case 0x09: // ldr rd, [pc, #imm8*4]
            {
                u32 v, x = (op >> 8) & 7;
                if (Read32(((pc + 4) & ~3u) + (op & 0xFF) * 4, v)) setv(x, v);
                else kill(x);
                break;
            }
            case 0x0A: case 0x0B: // register offset transfers: only loads matter
                if ((op >> 9) & 7) { if (((op >> 9) & 7) >= 3) kill(rd); }
                break;
            case 0x0C: store(rn, rd, ((op >> 6) & 31) * 4, 4); break; // str
            case 0x0E: store(rn, rd, (op >> 6) & 31, 1); break;       // strb
            case 0x10: store(rn, rd, ((op >> 6) & 31) * 2, 2); break; // strh
            case 0x0D: case 0x0F: case 0x11: kill(rd); break;         // ldr, ldrb, ldrh
            case 0x12: break;                                          // str sp
            case 0x13: case 0x14: case 0x15: kill((op >> 8) & 7); break; // ldr sp, add pc/sp
            case 0x16: case 0x17: // misc: push/pop, add sp
                if ((op & 0x0600) == 0x0400 && (op & 0x0800))
                {
                    if (op & 0x100) return; // pop {.., pc}
                    for (int i = 0; i < 8; i++) if (op & (1 << i)) kill(i);
                }
                break;
            case 0x18: // stmia rn!, {list}
            {
                u32 b = (op >> 8) & 7, list = op & 0xFF, cnt = 0;
                for (int i = 0; i < 8; i++) if (list & (1 << i)) cnt++;
                if (r.kind[b] == IO)
                {
                    if (touches(r.val[b], cnt * 4)) AddSiteAt(pc, true, depth);
                    r.val[b] += cnt * 4;
                }
                break;
            }
            case 0x19: // ldmia
            {
                u32 b = (op >> 8) & 7;
                for (int i = 0; i < 8; i++) if (op & (1 << i)) kill(i);
                kill(b);
                break;
            }
            case 0x1A: case 0x1B: // conditional branch, swi
                if ((op & 0x0F00) == 0x0F00) { r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = NONE; }
                break;
            case 0x1C: return; // b
            case 0x1E: // bl/blx prefix
            {
                u32 lo;
                if (!Read16(pc + 2, lo)) return;
                u32 hiOff = (u32)((int32_t)(op << 21) >> 9);
                if ((lo >> 11) == 0x1F || (lo >> 11) == 0x1D)
                {
                    u32 target = pc + 4 + hiOff + ((lo & 0x7FF) << 1);
                    bool toArm = (lo >> 11) == 0x1D;
                    Call(toArm ? target & ~3u : target, !toArm, r, depth);
                    r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = NONE;
                    pc += 2;
                }
                break;
            }
            default: break;
        }
        if (!AnyIo(r))
            return;
    }
}

void Scanner::Scan(u32 first, u32 count)
{
    for (u32 bi = first; bi < first + count && bi < _blockCount; bi++)
    {
        const Block& b = _blocks[bi];
        for (u32 off = 0; off + 4 <= b.size; off += 4)
        {
            const uint8_t* p = b.data + off;
            u32 w = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
            if ((w >> 28) == 0xF)
                continue;
            u32 rd = (w >> 12) & 0xF;
            if (rd >= 13)
                continue;
            u32 v;
            bool start = false;
            if ((w & 0x0F7F0000) == 0x051F0000)
            {
                // ldr rd, [pc, #+-imm]
                u32 imm = w & 0xFFF;
                u32 lit = b.runAddress + off + 8 + ((w & (1 << 23)) ? imm : -imm);
                start = Read32(lit, v) && isIo(v);
            }
            else if ((w & 0x0FEF0FFF) == 0x03A00301)
            {
                // mov rd, #0x04000000
                v = 0x04000000;
                start = true;
            }
            if (!start)
                continue;
            Regs r;
            for (int i = 0; i < 16; i++) r.kind[i] = NONE;
            r.kind[rd] = IO;
            r.val[rd] = v;
            // conditional loads still give the register on the taken path
            Trace(b.runAddress + off + 4, r, 0);
        }
        for (u32 off = 0; off + 4 <= b.size; off += 2)
        {
            const uint8_t* p = b.data + off;
            u32 h = p[0] | (p[1] << 8), h2 = p[2] | (p[3] << 8);
            u32 v = 0, x = (h >> 8) & 7, next = 2;
            bool start = false;
            if ((h & 0xF800) == 0x4800)
            {
                start = Read32(((b.runAddress + off + 4) & ~3u) + (h & 0xFF) * 4, v) && isIo(v);
            }
            else if ((h & 0xF8FF) == 0x2040 && h2 == ((20u << 6) | (x << 3) | x))
            {
                v = 0x04000000; start = true; next = 4; // movs rx, #0x40; lsls rx, rx, #20
            }
            else if ((h & 0xF8FF) == 0x2001 && h2 == ((26u << 6) | (x << 3) | x))
            {
                v = 0x04000000; start = true; next = 4; // movs rx, #1; lsls rx, rx, #26
            }
            if (!start)
                continue;
            Regs r;
            for (int i = 0; i < 16; i++) r.kind[i] = NONE;
            r.kind[x] = IO;
            r.val[x] = v;
            TraceThumb(b.runAddress + off + next, r, 0);
        }
    }
}

}
