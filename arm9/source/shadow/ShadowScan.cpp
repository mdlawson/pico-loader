#include "ShadowScan.h"

namespace shadow_scan
{
u32 gTargetArm7 = 0;

// Blocks keep their run address's alignment (the code sits at it on the ds), so
// whole halfwords and words read in one go: the loader scans with few cycles
typedef u32 __attribute__((may_alias)) u32a;
typedef uint16_t __attribute__((may_alias)) u16a;

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
    v = *(const u32a*)(b->data + (a - b->runAddress));
    return true;
}

bool Scanner::Read16(u32 a, u32& v) const
{
    if (a & 1)
        return false;
    const Block* b = BlockOf(a);
    if (!b || a - b->runAddress + 2 > b->size)
        return false;
    v = *(const u16a*)(b->data + (a - b->runAddress));
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
    {
        u32 op = 0;
        if (depth == 1 && (thumb ? Read16(site, op) : Read32(site, op)))
            AddCopy(site, thumb, op);
        return;
    }
    AddSite(site | (thumb ? 1 : 0));
}

void Scanner::Call(u32 target, bool thumb, const Regs& r, int depth, bool jump)
{
    // a call takes its io arguments along, a jump all its registers
    int carried = jump ? 13 : 4;
    bool args = false;
    for (int i = 0; i < carried; i++)
        if (r.kind[i] >= IO) args = true;
    if (!args || depth >= 2 || !BlockOf(target))
        return;
    u32 key = thumb | (jump ? 2 : 0);
    for (int i = 0; i < carried; i++)
        if (r.kind[i] >= IO) key = key * 31 + r.val[i] + i + r.kind[i] * 7;
    for (u32 i = 0; i < _callKeyN && i < 64; i++)
        if (_callKey[i][0] == target && _callKey[i][1] == key)
            return;
    _callKey[_callKeyN & 63][0] = target;
    _callKey[_callKeyN & 63][1] = key;
    _callKeyN++;
    Regs c;
    for (int i = 0; i < 16; i++) c.kind[i] = NONE;
    for (int i = 0; i < carried; i++) { c.kind[i] = r.kind[i]; c.val[i] = r.val[i]; }
    // a call from the io base's own code: where a copy routine's wrapper goes
    Context saved = _ctx;
    if (depth == 0)
    {
        _ctx.from = _from;
        _ctx.link = _link;
        _ctx.callee = target & ~1u;
        _ctx.calleeThumb = thumb;
        _ctx.ioArgs = 0;
        for (int i = 0; i < 4; i++)
            if (r.kind[i] >= IO) _ctx.ioArgs |= 1 << i;
        _ctx.dst = r.val[1];
    }
    _link = LINK_NONE;
    if (thumb) TraceThumb(target & ~1u, c, depth + 1);
    else Trace(target, c, depth + 1);
    _ctx = saved;
}

// A loop store in a routine called with an io address in r1 alone: the sdk's
// copies (src r0, dst r1, size r2) and fills (value r0, dst r1, size r2). A
// copy loads through r0 on the way to its store; a fill doesn't.
void Scanner::AddCopy(u32 site, bool thumb, u32 op)
{
    if (_ctx.link == LINK_NONE || _ctx.ioArgs != 2 || !_copies)
        return;
    // a b is a tail call only into what looks like a routine of its own: right
    // after another's return (else it may be the join of an if)
    if (_ctx.link == LINK_ARM_JUMP && !_ctx.calleeThumb)
    {
        u32 prev;
        if (!Read32(_ctx.callee - 4, prev) || (prev != 0xE12FFF1E && (prev & 0xFFFF8000) != 0xE8BD8000))
            return;
    }
    bool copy = false;
    for (u32 a = _ctx.callee; a < site && !copy; a += thumb ? 2 : 4)
    {
        u32 x;
        if (thumb)
        {
            if (!Read16(a, x)) break;
            // ldr/ldrb/ldrh imm, register-offset loads, ldmia: base in bits 5-3 (ldmia 10-8)
            if (((x >> 11) == 0x0D || (x >> 11) == 0x0F || (x >> 11) == 0x11) && ((x >> 3) & 7) == 0) copy = true;
            if ((x & 0xF000) == 0x5000 && (x & 0x0E00) >= 0x0600 && ((x >> 3) & 7) == 0) copy = true;
            if ((x & 0xF800) == 0xC800 && ((x >> 8) & 7) == 0) copy = true;
        }
        else
        {
            if (!Read32(a, x)) break;
            u32 xn = (x >> 16) & 0xF;
            bool load = x & (1 << 20);
            u32 t = (x >> 25) & 7;
            if (load && xn == 0 && (t == 2 || t == 3 || t == 4 || (t == 0 && (x & 0x90) == 0x90 && (x & 0x60))))
                copy = true;
        }
    }
    uint8_t kind;
    if (copy)
        kind = COPY;
    else if (thumb)
        kind = (op >> 11) == 0x0E || (op >> 9) == 0x2A ? FILL8 : (op >> 11) == 0x10 || (op >> 9) == 0x29 ? FILL16 : FILL32;
    else if ((op & 0x0E000000) == 0 && (op & 0x90) == 0x90)
        kind = FILL16;
    else
        kind = (op & 0x0C400000) == 0x04400000 ? FILL8 : FILL32;
    u32 at = _ctx.from | (_ctx.link == LINK_THUMB_CALL ? 1 : 0);
    for (u32 i = 0; i < _copyCount; i++)
        if (_copies[i].at == at)
            return;
    if (_copyCount >= _maxCopies)
    {
        _overflow++;
        return;
    }
    CopySite& c = _copies[_copyCount++];
    c.at = at;
    c.callee = _ctx.callee | (_ctx.calleeThumb ? 1 : 0);
    c.link = _ctx.link;
    c.kind = kind;
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
    for (int i = 0; i < 15; i++)
        if (i != 13 && r.kind[i] >= IO)
            return true;
    return false;
}

static inline bool isIo(u32 v) { return v >= 0x04000000 && v < 0x04002000; }
// a base with an unknown offset added that still most likely lands on a shadowed
// register: the sdk's setters get &REG_BG2PA + 0x10 * n and the like
static inline bool nearShadow(u32 v)
{
    u32 o = v - 0x04000000;
    if (shadow_scan::gTargetArm7)
        return o == 0 || (o >= 0x100 && o < 0x110) || (o >= 0x400 && o < 0x520); // + ch * 16 and the like
    return (o >= 0x10 && o < 0x58) || (o >= 0x1010 && o < 0x1058) || (o >= 0x330 && o < 0x3C0);
}

// does a store of size bytes at a touch a shadowed register?
static bool touches(u32 a, u32 size)
{
    for (u32 k = 0; k < size; k++)
        if (IsShadowed(a + k))
            return true;
    return false;
}

// a store through a base of this kind at a (its known part): a site? An io base
// plus something unknown is one on the arm9 (the setters' offsets are many);
// on the arm7 the unknown is a channel's 16 bytes, which keep the register
static inline bool hit(u32 kind, u32 a, u32 size)
{
    if (kind == 4 /* IOD */)
        return (a & 3) == 0 && a <= 0x04000008 && a + size > 0x04000008; // the channel's control, at +8
    return kind == 3 /* IOX */ && !shadow_scan::gTargetArm7 ? true : touches(a, size);
}

void Scanner::Trace(u32 pc, Regs r, int depth)
{
    u32 forks = 0;
    auto ok = [](u32 x) { return x < 13 || x == 14; }; // lr too: a leaf's spare register
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
                _from = pc; _link = LINK_ARM_CALL;
                Call(pc + 8 + ((u32)((int32_t)(op << 8) >> 6)) + ((op >> 23) & 2), true, r, depth);
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = r.kind[14] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            continue; // pld and the like
        }
        u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
        auto kill = [&](u32 x) { if (x < 16) r.kind[x] = NONE; };
        auto setv = [&](u32 x, u32 v) { if (ok(x)) { r.kind[x] = isIo(v) ? IO : CONST; r.val[x] = v; } };
        u32 t = (op >> 25) & 7;

        // branches
        if (t == 5)
        {
            if (op & 0x01000000)
            {
                // bl: follow the callee with the io arguments
                _from = pc; _link = LINK_ARM_CALL;
                Call(pc + 8 + ((u32)((int32_t)(op << 8) >> 6)), false, r, depth);
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = r.kind[14] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            if (al)
            {
                // on at the target, as a call that doesn't come back: a tail
                // call (ldr r1, =io / b MI_Copy32B) or the join of an if
                _from = pc; _link = LINK_ARM_JUMP;
                Call(pc + 8 + ((u32)((int32_t)(op << 8) >> 6)), false, r, depth, true);
                return;
            }
            // a conditional branch: on along the fall-through, and a short
            // forward one along its taken side too (the else of an if)
            u32 to = pc + 8 + ((u32)((int32_t)(op << 8) >> 6));
            if (to > pc && to - pc <= 0x100 && forks < 3 && _forkDepth < 2)
            {
                forks++; _forkDepth++;
                Trace(to, r, depth);
                _forkDepth--;
            }
            continue;
        }
        // bx / blx register
        if ((op & 0x0FFFFFD0) == 0x012FFF10)
        {
            u32 rm = op & 0xF;
            bool known = ok(rm) && r.kind[rm] == CONST;
            u32 to = r.val[rm];
            if (op & 0x20)
            {
                if (known)
                {
                    _from = pc; _link = al ? LINK_ARM_CALL : LINK_NONE;
                    Call(to, to & 1, r, depth);
                }
                r.kind[0] = r.kind[1] = r.kind[2] = r.kind[3] = r.kind[12] = r.kind[14] = NONE;
                if (!AnyIo(r))
                    return;
                continue;
            }
            if (al)
            {
                // ldr ip, =MI_Copy32B / ldr r1, =io / bx ip
                if (known)
                {
                    _from = pc; _link = LINK_ARM_JUMPREG;
                    Call(to, to & 1, r, depth, true);
                }
                return;
            }
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
            else if (ok(rn) && r.kind[rn] >= IO)
            {
                u32 cnt = 0;
                for (int i = 0; i < 16; i++)
                    if (list & (1 << i)) cnt++;
                u32 b = r.val[rn];
                u32 lo = up ? b + (pre ? 4 : 0) : b - 4 * cnt + (pre ? 0 : 4);
                if (hit(r.kind[rn], lo, 4 * cnt) && !(list & 0xE000))
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
            else if (ok(rn) && r.kind[rn] >= IO)
            {
                u32 b = r.val[rn];
                if (imm)
                {
                    u32 a = pre ? (up ? b + off : b - off) : b;
                    if (ok(rd) && hit(r.kind[rn], a, byte ? 1 : 4))
                        AddSiteAt(pc, false, depth);
                    if (wb || !pre)
                    {
                        if (al) r.val[rn] = up ? b + off : b - off;
                        else kill(rn);
                    }
                }
                else
                {
                    if (depth == 1 && touches(b, 0x40) && InLoop(pc, false))
                        AddCopy(pc, false, op);
                    if (wb || !pre)
                        kill(rn);
                }
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
                kill(rn);
                if (op & (3 << 23)) kill(rd); // long multiplies and swaps; plain mul/mla leave 15-12 alone
                if (!AnyIo(r)) return;
                continue;
            }
            bool load = op & (1 << 20), wb = op & (1 << 21), immForm = op & (1 << 22), up = op & (1 << 23), pre = op & (1 << 24);
            u32 off = ((op >> 4) & 0xF0) | (op & 0xF);
            bool store = !load && (sh == 1 || sh == 3); // strh, strd
            if (store && ok(rn) && r.kind[rn] >= IO)
            {
                u32 b = r.val[rn];
                if (immForm)
                {
                    u32 a = pre ? (up ? b + off : b - off) : b;
                    u32 size = sh == 1 ? 2 : 8;
                    bool regsOk = sh == 1 ? ok(rd) : (rd < 12 && !(rd & 1));
                    if (regsOk && hit(r.kind[rn], a, size))
                        AddSiteAt(pc, false, depth);
                    if (wb || !pre)
                    {
                        if (al) r.val[rn] = up ? b + off : b - off;
                        else kill(rn);
                    }
                }
                else
                {
                    // [rn, rm]: no site, but in a loop it makes a copy routine
                    if (depth == 1 && touches(b, 0x40) && InLoop(pc, false))
                        AddCopy(pc, false, op);
                    if (wb || !pre)
                        kill(rn);
                }
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
                if (ok(rm) && r.kind[rm] != NONE) { bv = r.val[rm]; haveB = true; }
            }
            if (rd == 15)
            {
                if (al) return;
                continue;
            }
            bool haveA = ok(rn) && r.kind[rn] != NONE;
            u32 av = haveA ? r.val[rn] : 0;
            u32 rmx = op & 0xF;
            bool looseB = t == 0 && (op & 0xFF0) == 0 && ok(rmx) && r.kind[rmx] == IOX;
            if (al && (opc == 4 || opc == 2 || opc == 12) && haveA && r.kind[rn] >= IO
                && (r.kind[rn] == IOX || !haveB) && nearShadow(av))
            {
                // io base + something unknown (or already so): still near the
                // shadow; a constant on top of the unknown still counts
                r.kind[rd] = IOX;
                r.val[rd] = r.kind[rn] == IOX && haveB ? (opc == 4 ? av + bv : opc == 2 ? av - bv : av | bv) : av;
            }
            else if (al && (opc == 4 || opc == 12) && !haveA && haveB && t == 1 && isIo(bv) && nearShadow(bv))
            {
                // something unknown + an io base: add r1, r4, #0x04000000
                r.kind[rd] = IOX;
                r.val[rd] = bv;
            }
            else if (al && opc == 13 && looseB)
            {
                r.kind[rd] = IOX; // mov of one
                r.val[rd] = bv;
            }
            else if (al && haveB && opc == 13) setv(rd, bv);
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
        if (rn < 8 && r.kind[rn] >= IO && hit(r.kind[rn], r.val[rn] + off, size))
            AddSiteAt(pc, true, depth);
        (void)rd;
    };
    u32 forks = 0;
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
                if (known(rn) && r.kind[rn] >= IO && nearShadow(r.val[rn])
                    && (r.kind[rn] == IOX || (!immf && !known(rm))))
                {
                    r.kind[rd] = IOX;
                    r.val[rd] = r.val[rn];
                }
                else if (known(rn) && (immf || known(rm)))
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
            case 0x0A: case 0x0B: // register offset transfers
                if (((op >> 9) & 7) >= 3) kill(rd);
                else if (depth == 1 && r.kind[rn] >= IO && touches(r.val[rn], 0x40) && InLoop(pc, true))
                    AddCopy(pc, true, op); // str/strh/strb [rn, rm] in a loop: a copy routine
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
                if (r.kind[b] >= IO)
                {
                    if (hit(r.kind[b], r.val[b], cnt * 4)) AddSiteAt(pc, true, depth);
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
                else if ((op & 0x0F00) != 0x0E00 && !(op & 0x80) && forks < 3 && _forkDepth < 2)
                {
                    // forward: along its taken side too
                    forks++; _forkDepth++;
                    TraceThumb(pc + 4 + ((op & 0xFF) << 1), r, depth);
                    _forkDepth--;
                }
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
                    _from = pc; _link = LINK_THUMB_CALL;
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

static inline bool al4(u32 w) { return (w >> 28) == 0xE; }

__attribute__((section(".itcm"))) void Scanner::Scan(u32 first, u32 count)
{
    for (u32 bi = first; bi < first + count && bi < _blockCount; bi++)
    {
        const Block& b = _blocks[bi];
        for (u32 off = 0; off + 4 <= b.size; off += 4)
        {
            const uint8_t* p = b.data + off;
            u32 w = *(const u32a*)p;
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
            bool unknownPlus = false, dmaBase = false;
            if (!start && !gTargetArm7 && al4(w) && (w & 0x0FE00FFF) == 0x02800301 && ((w >> 16) & 0xF) < 13)
            {
                // add rd, rn, #0x04000000: a dma channel's registers, rn = ch * 12 + 0xB0
                v = 0x04000000;
                start = dmaBase = true;
            }
            if (!start && gTargetArm7 && al4(w) && ((w & 0x0FE00FFF) == 0x02800301 || (w & 0x0FE00FFF) == 0x03800301)
                && ((w >> 16) & 0xF) < 13)
            {
                // add/orr rd, rn, #0x04000000: a channel's base, rn = ch << 4
                v = 0x04000000;
                start = unknownPlus = true;
            }
            if (!start)
                continue;
            Regs r;
            for (int i = 0; i < 16; i++) r.kind[i] = NONE;
            // the literals loaded just before, unconditionally: a jump target
            // (ldr ip, =MI_Copy32B / ldr r1, =io / bx ip)
            for (u32 k = 1; k <= 3 && off >= k * 4; k++)
            {
                const uint8_t* q = p - k * 4;
                u32 x = q[0] | (q[1] << 8) | (q[2] << 16) | ((u32)q[3] << 24);
                u32 xd = (x >> 12) & 0xF, xv;
                if ((x & 0xFF7F0000) != 0xE51F0000 || xd >= 13 || xd == rd)
                    break;
                u32 imm = x & 0xFFF;
                u32 at = b.runAddress + off - k * 4;
                if (Read32(at + 8 + ((x & (1 << 23)) ? imm : -imm), xv) && !isIo(xv) && r.kind[xd] == NONE)
                {
                    r.kind[xd] = CONST;
                    r.val[xd] = xv;
                }
            }
            r.kind[rd] = dmaBase ? IOD : unknownPlus ? IOX : IO;
            r.val[rd] = v;
            // conditional loads still give the register on the taken path
            Trace(b.runAddress + off + 4, r, 0);
        }
        const uint8_t* data = b.data;
        const u32 size = b.size;
        for (u32 off = 0; off + 4 <= size; off += 2)
        {
            const uint8_t* p = data + off;
            u32 h = *(const u16a*)p;
            // ldr rx, [pc, #imm] (0x48xx) or movs rx, #imm (0x20xx) only
            if ((h >> 11) != 9 && (h >> 11) != 4)
                continue;
            u32 h2 = *(const u16a*)(p + 2);
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
