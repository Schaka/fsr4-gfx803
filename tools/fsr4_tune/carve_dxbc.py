import sys,struct,hashlib,os,collections
p,out=sys.argv[1],sys.argv[2]; os.makedirs(out,exist_ok=True)
d=open(p,'rb').read(); i=0; n=0; tot=0; parts=collections.Counter(); sizes=[]
while True:
    i=d.find(b'DXBC',i)
    if i<0: break
    size=struct.unpack_from('<I',d,i+24)[0]; nch=struct.unpack_from('<I',d,i+28)[0]
    if 32<size<50_000_000 and nch<64 and i+size<=len(d):
        b=d[i:i+size]; h=hashlib.md5(b).hexdigest()[:16]
        for k in range(nch):
            off=struct.unpack_from('<I',b,32+4*k)[0]; parts[b[off:off+4]]+=1
        open(f'{out}/{i:08x}_{h}.dxbc','wb').write(b); n+=1; tot+=size; sizes.append(size); i+=size
    else: i+=4
sizes.sort()
print(p,'blobs',n,'bytes',tot,'min/med/max',sizes[0],sizes[len(sizes)//2],sizes[-1]); print(dict(parts))
