# SPDX-License-Identifier: LGPL-3.0-or-later
import re,sys
# Inputs, in the working directory: spec.txt (pdftotext -layout docs/reference/OpenSHMEM-1.6.pdf spec.txt)
# and syms.txt (nm -D --defined-only libosoe.so | awk '{print $2, $3}').
spec=open("spec.txt").read()
hdr=open('<repo>/shmem/include/shmem.h').read()
syms=set(l.split()[1] for l in open('syms.txt') if len(l.split())==2)
macros=set(re.findall(r'#define\s+(shmem_\w+)',hdr))
have=syms|macros
# split into examples
parts=re.split(r'\n\s*Example (\d+)\.',spec)
res=[]
for i in range(1,len(parts),2):
    n=int(parts[i]); body=parts[i+1]
    # cut at next section heading "9.x.y  SHMEM_" or "Annex"
    m=re.search(r'\n(9|10)\.\d+(\.\d+)*\s+[A-Z_]{4,}|\nAnnex ',body)
    if m: body=body[:m.start()]
    if '#include' not in body and 'shmem_init' not in body: continue
    code=body
    calls=set(re.findall(r'\b(shmem_\w+|start_pes|shmalloc|_my_pe|_num_pes)\s*\(',code))
    missing=sorted(c for c in calls if c not in have)
    static=bool(re.search(r'\bstatic\s+(?!inline)(?:const\s+)?(?:unsigned\s+)?(?:int|long|short|char|float|double|uint64_t|size_t|int\w*_t|shmem_\w+|struct)',code))
    res.append((n,missing,static))
ok=[r for r in res if not r[1] and not r[2]]
print("examples with code:",len(res))
print("all routines present AND no static symmetric object:",[r[0] for r in ok])
print("all routines present but uses statics:",[r[0] for r in res if not r[1] and r[2]])
from collections import Counter
c=Counter(m for r in res for m in r[1])
print(c.most_common(40))
