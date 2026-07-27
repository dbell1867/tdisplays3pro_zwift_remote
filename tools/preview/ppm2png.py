import sys, zlib, struct
d = open(sys.argv[1],'rb').read()
# parse P6 header
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
raw = parts[3]
rows = b''.join(b'\x00' + raw[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, b):
    c = t + b
    return struct.pack('>I', len(b)) + c + struct.pack('>I', zlib.crc32(c))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(rows, 9))
       + chunk(b'IEND', b''))
open(sys.argv[2],'wb').write(png)
print(f"{sys.argv[2]}  {w}x{h}")
