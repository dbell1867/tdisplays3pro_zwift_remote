import sys, zlib, struct
d=open(sys.argv[1],'rb').read(); p=d.split(b'\n',3); w,h=map(int,p[1].split()); raw=p[3]
S=int(sys.argv[3]); x0,y0,x1,y1=(map(int,sys.argv[4].split(','))) if len(sys.argv)>4 else (0,0,w,h)
cw,ch=x1-x0,y1-y0
rows=b''
for y in range(y0,y1):
    line=b''.join(raw[(y*w+x)*3:(y*w+x)*3+3]*S for x in range(x0,x1))
    rows+=(b'\x00'+line)*S
def ck(t,b): c=t+b; return struct.pack('>I',len(b))+c+struct.pack('>I',zlib.crc32(c))
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ck(b'IHDR',struct.pack('>IIBBBBB',cw*S,ch*S,8,2,0,0,0))+ck(b'IDAT',zlib.compress(rows,9))+ck(b'IEND',b''))
print(sys.argv[2], cw*S, ch*S)
