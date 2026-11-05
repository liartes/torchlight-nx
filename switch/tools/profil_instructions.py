#!/usr/bin/env python3
"""Échantillons du thread principal classés par type d instruction : coûts diffus invisibles dans un profil par fonction.

Usage : profil_instructions.py <ELF du .nro testé> <dossier> ; le dossier contient combat.txt, ville.txt et mine.txt, tranches de
rex_pilas.txt (découpées avec le nombre d échantillons « muestras » de chaque bilan de rex_perfil.log).
"""
import struct,sys,collections as C
elf,S=sys.argv[1:3]
d=open(elf,"rb").read()
phoff=struct.unpack_from("<Q",d,32)[0];phsz,phn=struct.unpack_from("<HH",d,54)
segs=[]
for i in range(phn):
    t,fl,off,va,pa,fs,ms,al=struct.unpack_from("<IIQQQQQQ",d,phoff+i*phsz)
    if t==1: segs.append((va,fs,off,fl))
def mot(a):
    for va,fs,off,fl in segs:
        if va<=a<va+fs-3: return struct.unpack_from("<I",d,off+a-va)[0]
    return None
def cl(w):
    if w is None: return "hors image"
    if w&0xFFFFFFE0==0xD51B4400: return "msr fpcr"
    if w&0xFFFFFFE0==0xD53B4400: return "mrs fpcr"
    if w&0xFFFFFC1F==0xD63F0000: return "blr"
    if w&0xFFFFFC1F==0xD61F0000: return "br"
    if w&0xFFFFFC1F==0xD65F0000: return "ret"
    if w&0xFC000000==0x94000000: return "bl"
    if w&0xFC000000==0x14000000: return "b"
    if w&0xFF000010==0x54000000: return "b.cond"
    if w&0x7E000000==0x34000000: return "cbz/cbnz"
    if w&0x7E000000==0x36000000: return "tbz/tbnz"
    if w==0xD4000001 or w&0xFFE0001F==0xD4000001: return "svc"
    if w&0xFFFFF0DF==0xD50330BF or w&0xFFFFF09F==0xD503309F: return "dmb/dsb"
    if w&0xFFFFF0FF==0xD50330DF: return "isb"
    if w&0xFFFFFC00 in(0xDAC00C00,0x5AC00800,0xDAC00800,0x5AC00400,0xDAC00400): return "rev"
    if w&0x3F000000==0x08000000: return "exclusif/atomique"
    if w&0x0A000000==0x08000000:  # chargements et rangements
        simd=(w>>26)&1
        if (w>>24)&0x3B==0x28 or (w>>24)&0x3B==0x29 or (w>>24)&0x3B==0x2D:  # paires, approximation
            pass
        load=(w>>22)&1
        return ("lecture" if load else "écriture")+(" flottante/vecteur" if simd else " entière")
    if w&0x5F000000==0x1E000000:
        if w&0xFF20FC00==0x1E201800: return "fdiv"
        if w&0xFF3FFC00==0x1E21C000: return "fsqrt"
        return "flottant scalaire"
    if w&0x9F000000==0x0E000000 or w&0xDF000000==0x4E000000 or w&0xBF000000==0x0E000000 or w&0xFF000000==0x6E000000 or w&0xFF000000==0x2E000000: return "vecteur"
    return "autre"
def lire(n):
    o=[]
    for l in open(f"{S}/{n}.txt",errors="ignore"):
        if not l.startswith("Main XThread (F|"): continue
        o.append(int(l.split("|",1)[1].split()[0],16))
    return o
print(f"{'instruction à l adresse':<28}{'combat':>8}{'ville':>8}{'mine':>8}   |  {'instruction précédente':<26}{'combat':>8}{'ville':>8}{'mine':>8}")
R={n:lire(n) for n in("combat","ville","mine")}
A={n:C.Counter(cl(mot(p)) for p in v) for n,v in R.items()}
B={n:C.Counter(cl(mot(p-4)) for p in v) for n,v in R.items()}
ka=[k for k,_ in A["combat"].most_common(16)];kb=[k for k,_ in B["combat"].most_common(16)]
T={n:len(v) for n,v in R.items()}
for i in range(16):
    a=ka[i] if i<len(ka) else "";b=kb[i] if i<len(kb) else ""
    s=f"{a:<28}"+"".join(f"{100*A[n][a]/T[n]:>8.2f}" for n in R) if a else " "*52
    print(s+"   |  "+(f"{b:<26}"+"".join(f"{100*B[n][b]/T[n]:>8.2f}" for n in R) if b else ""))
print("\nmsr/mrs fpcr : échantillons sur ou juste après, autour (combat)")
pc=R["combat"]
msr=sum(1 for p in pc if cl(mot(p))=="msr fpcr" or cl(mot(p-4))=="msr fpcr")
mrs=sum(1 for p in pc if cl(mot(p))=="mrs fpcr" or cl(mot(p-4))=="mrs fpcr")
print(f"  msr {100*msr/len(pc):.2f} %, mrs {100*mrs/len(pc):.2f} %")
# nombre statique dans le code
va,fs,off,fl=segs[0]
import array
a=array.array("I");a.frombytes(d[off:off+(fs&~3)])
print("  sites statiques dans .text :","msr fpcr",sum(1 for w in a if w&0xFFFFFFE0==0xD51B4400),", mrs fpcr",sum(1 for w in a if w&0xFFFFFFE0==0xD53B4400))
# Lectures : quelles adresses chaudes ?
import itertools
c=C.Counter(pc)
print("\nadresses les plus chaudes (combat) :")
for p,v in c.most_common(10):
    print(f"  {p:#x} {100*v/len(pc):.2f} %  {cl(mot(p))}  (avant : {cl(mot(p-4))})")
