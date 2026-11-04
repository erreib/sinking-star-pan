#!/usr/bin/env python3
"""Dump Jai Type_Info structs from a PE image (sinking_star.exe).

Usage:
  jai_types.py EXE list [SUBSTR]        list struct/enum type names (optionally filtered)
  jai_types.py EXE struct NAME [NAME..] dump members of the named struct(s)
  jai_types.py EXE enum NAME            dump enum values
  jai_types.py EXE raw NAME             raw qword dump of the Type_Info
"""
import struct, sys, re

TAGS = {0:'INTEGER',1:'FLOAT',2:'BOOL',3:'STRING',4:'POINTER',5:'PROCEDURE',6:'VOID',7:'STRUCT',
        8:'ARRAY',9:'OVERLOAD_SET',10:'ANY',11:'ENUM',12:'POLYMORPHIC_VARIABLE',13:'TYPE',14:'CODE',
        15:'UNTYPED_LITERAL?',16:'UNTYPED_ENUM?',17:'VARIANT'}

class PE:
    def __init__(self, path):
        self.d = open(path,'rb').read()
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe+6)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<Q', d, opt+24)[0]
        optsz = struct.unpack_from('<H', d, pe+20)[0]
        self.secs = []
        for i in range(nsec):
            s = opt + optsz + i*40
            name = d[s:s+8].rstrip(b'\0').decode()
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s+8)
            self.secs.append((name, va, vsize, raw, rsize))
    def va2off(self, va):
        rva = va - self.base
        for name, sva, vsize, raw, rsize in self.secs:
            if sva <= rva < sva + max(vsize, rsize):
                off = rva - sva
                if off < rsize: return raw + off
                return None
        return None
    def valid(self, va): return self.va2off(va) is not None
    def u64(self, va):
        o = self.va2off(va); return struct.unpack_from('<Q', self.d, o)[0] if o is not None else None
    def s64(self, va):
        o = self.va2off(va); return struct.unpack_from('<q', self.d, o)[0] if o is not None else None
    def u32(self, va):
        o = self.va2off(va); return struct.unpack_from('<I', self.d, o)[0] if o is not None else None
    def f32(self, va):
        o = self.va2off(va); return struct.unpack_from('<f', self.d, o)[0] if o is not None else None
    def jstr(self, va):
        """Jai string {count s64, data *u8} at va."""
        n = self.s64(va); p = self.u64(va+8)
        if n is None or p is None or n < 0 or n > 256: return None
        if n == 0: return ''
        o = self.va2off(p)
        if o is None: return None
        b = self.d[o:o+n]
        try: return b.decode('utf-8')
        except UnicodeDecodeError: return None

def find_all_types(pe):
    """Scan .data/.rdata for Type_Info_Struct / Type_Info_Enum headers: tag at +0 (u32), runtime_size
    at +8, name string at +0x10. Returns {name: [va,...]}."""
    out = {}
    for name, sva, vsize, raw, rsize in pe.secs:
        if name not in ('.data', '.rdata'): continue
        d = pe.d
        for off in range(raw, raw + rsize - 0x40, 8):
            tag = struct.unpack_from('<I', d, off)[0]
            if tag not in (7, 11): continue
            size = struct.unpack_from('<q', d, off+8)[0]
            if size < 0 or size > 0x100000: continue
            va = pe.base + sva + (off - raw)
            s = pe.jstr(va + 0x10)
            if not s or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', s): continue
            out.setdefault(s, []).append(va)
    return out

def type_name(pe, tva, depth=0):
    if tva is None or not pe.valid(tva): return '?'
    tag = pe.u32(tva); size = pe.s64(tva+8)
    if tag == 0:
        signed = pe.u32(tva+0x10) if pe.valid(tva+0x10) else 0
        return ('s' if signed & 1 else 'u') + str(size*8)
    if tag == 1: return 'float' if size == 4 else 'float64'
    if tag == 2: return 'bool'
    if tag == 3: return 'string'
    if tag == 6: return 'void'
    if tag == 4:
        return '*' + (type_name(pe, pe.u64(tva+0x10), depth+1) if depth < 4 else '…')
    if tag in (7, 11):
        return pe.jstr(tva+0x10) or ('struct' if tag == 7 else 'enum')
    if tag == 8:
        kind = pe.u32(tva+0x10); count = pe.s64(tva+0x18); elem = pe.u64(tva+0x20)
        et = type_name(pe, elem, depth+1) if depth < 4 else '…'
        return (f'[{count}]' if kind == 0 else '[]' if kind == 1 else '[..]') + et
    if tag == 5: return 'proc'
    if tag == 10: return 'Any'
    if tag == 13: return 'Type'
    return TAGS.get(tag, f'tag{tag}')

def dump_struct(pe, va, show_all=True):
    name = pe.jstr(va+0x10); size = pe.s64(va+8)
    mcount = pe.s64(va+0x40); mptr = pe.u64(va+0x48)
    print(f'=== {name}  Type_Info rva=0x{va-pe.base:x}  size=0x{size:x}  members={mcount}')
    if mcount is None or mcount < 0 or mcount > 20000 or not pe.valid(mptr):
        print('  (members array unreadable)'); return
    for i in range(mcount):
        m = mptr + i*0x40
        mname = pe.jstr(m); mtype = pe.u64(m+0x10); moff = pe.s64(m+0x18); flags = pe.u32(m+0x20)
        tn = type_name(pe, mtype)
        tsize = pe.s64(mtype+8) if pe.valid(mtype) else -1
        fl = []
        if flags & 1: fl.append('const')
        if flags & 2: fl.append('imported')
        if flags & 4: fl.append('using')
        if flags & 8: fl.append('as')
        if flags & 0x10: fl.append('place')
        ncount = pe.s64(m+0x28); nptr = pe.u64(m+0x30)
        notes = []
        if ncount and 0 < ncount < 16 and pe.valid(nptr):
            notes = [pe.jstr(nptr+j*16) or '?' for j in range(ncount)]
        offs = f'+0x{moff:04x}' if moff >= 0 else 'const  '
        print(f'  {offs} {mname:40s} {tn:32s} size={tsize:<6} {" ".join(fl)} {" ".join("@"+n for n in notes)}')

def dump_enum(pe, va):
    name = pe.jstr(va+0x10)
    # Type_Info_Enum: info(16) name(16) internal_type ptr(8) names [] (16) values [] (16) ...
    it = pe.u64(va+0x20); ncount = pe.s64(va+0x28); nptr = pe.u64(va+0x30)
    vcount = pe.s64(va+0x38); vptr = pe.u64(va+0x40)
    print(f'=== enum {name} rva=0x{va-pe.base:x} internal={type_name(pe,it)} names={ncount} values={vcount}')
    if ncount is None or ncount < 0 or ncount > 5000: return
    for i in range(ncount):
        n = pe.jstr(nptr + i*16); v = pe.s64(vptr + i*8) if vcount == ncount else '?'
        print(f'  {v:>6} {n}')

def main():
    pe = PE(sys.argv[1]); cmd = sys.argv[2]
    types = find_all_types(pe)
    if cmd == 'list':
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for n in sorted(types):
            if sub in n.lower():
                vas = types[n]
                print(f'{n:50s} {TAGS[pe.u32(vas[0])]:7s} size=0x{pe.s64(vas[0]+8):x} x{len(vas)}')
    elif cmd == 'struct':
        for n in sys.argv[3:]:
            for va in types.get(n, []):
                if pe.u32(va) == 7: dump_struct(pe, va)
            if n not in types: print(f'(no type named {n})')
    elif cmd == 'enum':
        for va in types.get(sys.argv[3], []):
            if pe.u32(va) == 11: dump_enum(pe, va)
    elif cmd == 'raw':
        for va in types.get(sys.argv[3], []):
            for i in range(0, 0x80, 8):
                print(f'  +0x{i:02x} 0x{pe.u64(va+i):016x}')
    elif cmd == 'grep':
        sub = sys.argv[3].lower()
        for n in sorted(types):
            for va in types[n]:
                if pe.u32(va) == 7:
                    mcount = pe.s64(va+0x40); mptr = pe.u64(va+0x48)
                    if mcount is None or mcount < 0 or mcount > 20000 or not pe.valid(mptr): continue
                    for i in range(mcount):
                        m = mptr + i*0x40; mn = pe.jstr(m) or ''
                        if sub in mn.lower():
                            print(f'{n}.{mn}  +0x{pe.s64(m+0x18):x}  {type_name(pe, pe.u64(m+0x10))}')
                elif pe.u32(va) == 11:
                    ncount = pe.s64(va+0x28); nptr = pe.u64(va+0x30)
                    if ncount is None or ncount < 0 or ncount > 5000 or not pe.valid(nptr): continue
                    for i in range(ncount):
                        en = pe.jstr(nptr+i*16) or ''
                        if sub in en.lower(): print(f'enum {n}.{en}')
    elif cmd == 'big':
        lim = int(sys.argv[3], 0)
        for n in sorted(types, key=lambda k: -pe.s64(types[k][0]+8)):
            sz = pe.s64(types[n][0]+8)
            if sz >= lim: print(f'{n:50s} size=0x{sz:x}')
    elif cmd == 'count':
        print(len(types), 'named types')

if __name__ == '__main__': main()
