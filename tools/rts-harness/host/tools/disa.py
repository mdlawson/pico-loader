import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
g, a, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
t = a & 1; a &= ~1
ram = open(f'out/dumps/{g}.ram','rb').read(); itcm = open(f'out/dumps/{g}.itcm','rb').read()
buf, base = (itcm, 0x01FF8000) if a < 0x02000000 else (ram, 0x02000000)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if t else CS_MODE_ARM)
for i in md.disasm(buf[a-base:a-base+n], a): print(f'{i.address:08X} {i.bytes[::-1].hex():8} {i.mnemonic} {i.op_str}')
