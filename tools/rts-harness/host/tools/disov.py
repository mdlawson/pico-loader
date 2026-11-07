import struct,sys
G=sys.argv[1]; ov=int(sys.argv[2]); A=int(sys.argv[3],16); N=int(sys.argv[4])
sys.argv=['x',G]
exec(open('tools/ioscan.py').read().split('def scanRegions')[0])
o=[o for o in ovs if o[0]==ov][0]; d=ovlData(o)
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB if A&1 else CS_MODE_ARM); A&=~1
for i in md.disasm(d[A-o[1]:A-o[1]+N], A): print(f'{i.address:08X} {i.mnemonic} {i.op_str}')
