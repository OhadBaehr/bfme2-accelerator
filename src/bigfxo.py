# bigfxo.py <out dir> <folder>...: every *.fxo inside the .big archives under the folders (and loose ones), one copy per archive
import sys, os, struct, io
out = sys.argv[1]
n = 0
for root in sys.argv[2:]:
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            fl = f.lower()
            if fl.endswith('.fxo'):
                d = os.path.join(out, '_loose', os.path.relpath(dp, root).replace(os.sep, '_'))
                os.makedirs(d, exist_ok=True)
                io.open(os.path.join(d, f), 'wb').write(io.open(p, 'rb').read()); n += 1
                continue
            if not fl.endswith('.big'): continue
            try:
                fh = io.open(p, 'rb'); head = fh.read(16)
            except Exception as e:
                print('cannot open', p, e); continue
            if head[:4] not in (b'BIGF', b'BIG4'): fh.close(); continue
            cnt, first = struct.unpack('>II', head[8:16])
            table = fh.read(first - 16) if first > 16 else b''
            pos = 0; ents = []
            for i in range(cnt):
                if pos + 8 > len(table): break
                off, size = struct.unpack('>II', table[pos:pos + 8]); pos += 8
                e = table.find(b'\0', pos)
                if e < 0: break
                name = table[pos:e].decode('latin-1'); pos = e + 1
                ents.append((name, off, size))
            got = 0
            for name, off, size in ents:
                if not name.lower().endswith('.fxo'): continue
                fh.seek(off); data = fh.read(size)
                d = os.path.join(out, os.path.basename(root.rstrip('/\\')) + '__' + f)
                os.makedirs(d, exist_ok=True)
                io.open(os.path.join(d, os.path.basename(name.replace('\\', '/'))), 'wb').write(data); got += 1; n += 1
            fh.close()
            if got: print('%-60s %d effects' % (p, got))
print(n, 'effect files written to', out)
