#!/usr/bin/env python3
"""Deathsmiles iOS -> Vita: converts the armv7 Mach-O slice into game.bin for the loader.
(Optional PC tool: the port does the same conversion on the Vita on first launch, see loader/src/convert.c.)
Formato game.bin (little endian):
  magic 'DSLD', version u32, vmbase u32, imgsize u32 (bytes to copy), vmsize u32 (total including zerofill),
  entry(main) u32, ninit u32, nreloc u32, nimport u32, strtab_size u32
  init[ninit] u32 (vmaddr)
  reloc[nreloc] u32 (vmaddr of words the slide is added to)
  import[nimport] {u32 vmaddr, i32 addend, u32 name_off, u32 flags(bit0=dato, bit1=weak)}
  strtab (nombres terminados en 0)
  imagen (imgsize bytes, desde vmbase)
"""
import struct, sys

def uleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1; r |= (x & 0x7f) << s; s += 7
        if not x & 0x80: return r, i
def sleb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1; r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            if x & 0x40: r -= 1 << s
            return r, i

DATA_PREFIX = ('_OBJC_CLASS_$_', '_OBJC_METACLASS_$_', '__NSConcrete', '_k', '___stack_chk_guard',
               '___stderrp', '___stdoutp', '___stdinp', '_NSLocalizedDescriptionKey', '___CFConstantStringClassReference',
               '_GCController', '_UIApplication', '_NS', '__objc_empty', '_UIKeyboard', '_UITextField', '_UITextView', '_UIScreen')
DATA_EXACT = {'__NSConcreteStackBlock', '__NSConcreteGlobalBlock', '__DefaultRuneLocale', '_vm_page_size', '_mach_task_self_', '__dispatch_main_q', '_NSGenericException', '__objc_empty_cache', '__objc_empty_vtable', '_NXArgc', '_NXArgv', '_environ', '___progname'}
def is_data(n):
    if n in DATA_EXACT: return True
    if n.startswith(('_NS', '_UI', '_GC')) and not n.endswith(('Log', 'Main')) and ('Notification' in n or 'Key' in n or 'Name' in n or 'Mode' in n):
        return True
    return n.startswith(DATA_PREFIX[:10])

def main(src, dst):
    d = open(src, 'rb').read()
    assert struct.unpack('<I', d[:4])[0] == 0xfeedface, 'expected a 32-bit Mach-O (armv7 slice)'
    ncmds = struct.unpack('<I', d[16:20])[0]
    off = 28; segs = []; dyld = None; sects = {}; entry_pc = None
    for _ in range(ncmds):
        cmd, size = struct.unpack('<II', d[off:off+8])
        if cmd == 1:  # LC_SEGMENT
            name = d[off+8:off+24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack('<4I', d[off+24:off+40])
            nsect = struct.unpack('<I', d[off+48:off+52])[0]
            segs.append((name, vmaddr, vmsize, fileoff, filesize))
            so = off + 56
            for _ in range(nsect):
                sn = d[so:so+16].rstrip(b'\0').decode(); seg = d[so+16:so+32].rstrip(b'\0').decode()
                a, s = struct.unpack('<II', d[so+32:so+40]); sects[(seg, sn)] = (a, s); so += 68
        elif cmd == 0x80000022:  # LC_DYLD_INFO_ONLY
            dyld = struct.unpack('<10I', d[off+8:off+48])
        elif cmd == 5:  # LC_UNIXTHREAD: flavor,count, r0..r15
            regs = struct.unpack('<16I', d[off+16:off+80]); entry_pc = regs[15]
        off += size
    segaddr = [s[1] for s in segs]
    body = [s for s in segs if s[0] not in ('__PAGEZERO', '__LINKEDIT')]
    vmbase = min(s[1] for s in body); vmend = max(s[1] + s[2] for s in body)
    img = bytearray(max(s[1] + s[4] for s in body) - vmbase)
    for name, va, vs, fo, fs in body:
        img[va - vmbase: va - vmbase + fs] = d[fo:fo+fs]

    # rebases
    rb_off, rb_size = dyld[0], dyld[1]; b = d[rb_off:rb_off+rb_size]; i = 0; seg = 0; o = 0; relocs = []
    while i < len(b):
        x = b[i]; i += 1; op = x & 0xf0; imm = x & 0x0f
        if op == 0x00: break
        elif op == 0x10: pass  # SET_TYPE
        elif op == 0x20: seg = imm; o, i = uleb(b, i)
        elif op == 0x30: v, i = uleb(b, i); o = (o + v) & 0xffffffff
        elif op == 0x40: o += imm * 4
        elif op == 0x50:
            for _ in range(imm): relocs.append(segaddr[seg] + o); o += 4
        elif op == 0x60:
            n, i = uleb(b, i)
            for _ in range(n): relocs.append(segaddr[seg] + o); o += 4
        elif op == 0x70: relocs.append(segaddr[seg] + o); v, i = uleb(b, i); o += 4 + v
        elif op == 0x80:
            n, i = uleb(b, i); sk, i = uleb(b, i)
            for _ in range(n): relocs.append(segaddr[seg] + o); o += 4 + sk
        else: raise SystemExit('rebase opcode desconocido %x' % op)

    imports = []
    def binds(boff, bsize, lazy, weak):
        b = d[boff:boff+bsize]; i = 0; seg = 0; o = 0; sym = ''; add = 0
        while i < len(b):
            x = b[i]; i += 1; op = x & 0xf0; imm = x & 0x0f
            if op == 0x00:
                if lazy: continue
                break
            if op == 0x10: pass
            elif op == 0x20: _, i = uleb(b, i)
            elif op == 0x30: pass
            elif op == 0x40:
                e = d.index(b'\0', boff + i); sym = d[boff+i:e].decode(); i = e - boff + 1
            elif op == 0x50: pass
            elif op == 0x60: add, i = sleb(b, i)
            elif op == 0x70: seg = imm; o, i = uleb(b, i)
            elif op == 0x80: v, i = uleb(b, i); o = (o + v) & 0xffffffff
            elif op == 0x90: imports.append((segaddr[seg]+o, add, sym, weak)); o += 4
            elif op == 0xa0: imports.append((segaddr[seg]+o, add, sym, weak)); v, i = uleb(b, i); o += 4 + v
            elif op == 0xb0: imports.append((segaddr[seg]+o, add, sym, weak)); o += 4 + imm * 4
            elif op == 0xc0:
                n, i = uleb(b, i); sk, i = uleb(b, i)
                for _ in range(n): imports.append((segaddr[seg]+o, add, sym, weak)); o += 4 + sk
            else: raise SystemExit('bind opcode desconocido %x' % op)
    binds(dyld[2], dyld[3], False, False)
    binds(dyld[4], dyld[5], False, True)
    binds(dyld[6], dyld[7], True, False)

    a, s = sects.get(('__DATA', '__mod_init_func'), (0, 0))
    inits = [struct.unpack('<I', img[a - vmbase + k: a - vmbase + k + 4])[0] for k in range(0, s, 4)]
    names = sorted({x[2] for x in imports}); stro = {}; strtab = bytearray()
    for n in names: stro[n] = len(strtab); strtab += n.encode() + b'\0'
    MAIN = 0x14499  # main() in Thumb, found by disassembly (start @0xb820 -> blx 0x14498)
    want = ['__objc_classlist', '__objc_nlclslist', '__objc_catlist', '__objc_selrefs', '__objc_classrefs',
            '__objc_superrefs', '__cfstring', '__objc_protolist', '__objc_imageinfo', '__program_vars']
    secs = [(n, *sects[('__DATA', n)]) for n in want if ('__DATA', n) in sects]
    out = bytearray(struct.pack('<4s10I', b'DSLD', 2, vmbase, len(img), vmend - vmbase, MAIN, len(inits), len(relocs), len(imports), len(strtab), len(secs)))
    for n, a, sz in secs: out += struct.pack('<16sII', n.encode()[:16], a, sz)
    out += b''.join(struct.pack('<I', x) for x in inits)
    out += b''.join(struct.pack('<I', x) for x in relocs)
    for va, add, sym, weak in imports:
        out += struct.pack('<IiII', va, add, stro[sym], (1 if is_data(sym) else 0) | (2 if weak else 0))
    out += strtab; out += img
    open(dst, 'wb').write(out)
    print(f'vmbase {vmbase:#x} vmsize {vmend-vmbase:#x} img {len(img):#x} entry(start) {entry_pc:#x} main {MAIN:#x}')
    print(f'inits {[hex(x) for x in inits]} relocs {len(relocs)} imports {len(imports)} symbols {len(names)}')
    nd = [n for n in names if is_data(n)]
    print('data symbols:', len(nd), ' '.join(nd))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
