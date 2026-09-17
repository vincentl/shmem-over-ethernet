import sys
from collections import deque
OPEN,SEALED,SENDING,FREE,DRAIN="OPEN","SEALED","SENDING","FREE","DRAINING"
def run(seal,openrule,drain,seqat,sizes,cap,R=1,wfail="other",maxst=2_000_000):
    nW=len(sizes)
    # buf: st,head,bit,filled,seq,msgs
    b0=(OPEN,0,0,0,0,()); b1=(FREE,0,1 if seal=="headcas" else 0,0,-1,())
    init=((b0,b1),tuple((0,0,0,0,0,-9) for _ in range(nW)),(),None,1,(),(),R)
    V={}; seen={init:None}; q=deque([init]); edges={}
    def sb(bs,k,v): l=list(bs); l[k]=v; return tuple(l)
    def rec(k,S,m):
        if k not in V: V[k]=(S,m)
    total=sum(len(s) for s in sizes)+R
    while q:
        S=q.popleft(); bufs,ws,sq,inf,ns,wire,srs,dl=S; out=[]
        for w in range(nW):
            pc,i,b,h,off,sr=ws[w]
            if i>=len(sizes[w]): continue
            n=sizes[w][i]; st,hd,bit,fl,seq,msgs=bufs[b]
            def W(nw,**kw): return sb(ws,w,nw)
            if pc==0: out.append((f"W{w} load b{b} head={hd}{'|S' if bit else ''}",(bufs,sb(ws,w,(1,i,b,hd*2+bit,0,-9)),sq,inf,ns,wire,srs,dl)))
            elif pc==1:
                hh,hb=divmod(h,2)
                if st==OPEN and hb==0 and hh+n<=cap: out.append((f"W{w} check ok b{b}",(bufs,sb(ws,w,(2,i,b,h,0,-9)),sq,inf,ns,wire,srs,dl)))
                else:
                    nb=1-b if wfail=="other" else b
                    out.append((f"W{w} fail -> b{nb}",(bufs,sb(ws,w,(0,i,nb,0,0,-9)),sq,inf,ns,wire,srs,dl)))
            elif pc==2:
                hh,hb=divmod(h,2)
                if hd==hh and bit==hb:
                    if st==DRAIN: rec("DRAINRES",S,"")
                    for m in msgs:
                        if not(hh+n<=m[2] or m[2]+m[3]<=hh): rec("OVERLAP",S,"")
                    nb=(st,hd+n,bit,fl,seq,msgs+((w,i,hh,n,False),))
                    out.append((f"W{w} CAS ok off={hh} b{b} (state {st})",(sb(bufs,b,nb),sb(ws,w,(3,i,b,h,hh,-9)),sq,inf,ns,wire,srs,dl)))
                else: out.append((f"W{w} CAS fail",(bufs,sb(ws,w,(0,i,b,0,0,-9)),sq,inf,ns,wire,srs,dl)))
            elif pc==3: out.append((f"W{w} read seq={seq}",(bufs,sb(ws,w,(4,i,b,h,off,seq)),sq,inf,ns,wire,srs+((w,i,seq),),dl)))
            elif pc==4:
                nm=tuple((a,c,o,d,True if (a,c,o)==(w,i,off) else e) for (a,c,o,d,e) in msgs)
                out.append((f"W{w} memcpy",(sb(bufs,b,(st,hd,bit,fl,seq,nm)),sb(ws,w,(5,i,b,h,off,sr)),sq,inf,ns,wire,srs,dl)))
            elif pc==5:
                nb=(st,hd,bit,fl+n,seq,msgs)
                if fl+n>hd: rec("ACCT",S,"")
                out.append((f"W{w} filled+={n}",(sb(bufs,b,nb),sb(ws,w,(0,i+1,b,0,0,-9)),sq,inf,ns,wire,srs,dl)))
        for b in (0,1):
            st,hd,bit,fl,seq,msgs=bufs[b]; ost=bufs[1-b][0]
            if st==OPEN and hd>0 and b not in sq:
                nseq,nns=(ns,ns+1) if seqat=="seal" else (seq,ns)
                nb=(SEALED,hd,1 if seal=="headcas" else bit,fl,nseq,msgs)
                out.append((f"PT seal b{b}"+(f" seq={nseq}" if seqat=="seal" else ""),(sb(bufs,b,nb),ws,sq+(b,),inf,nns,wire,srs,dl)))
            if st==FREE and (openrule in ("any","cqe") or ost not in (OPEN,DRAIN)):
                if msgs: lostS=True
                else: lostS=False
                nst=DRAIN if drain else OPEN
                nbit=1 if (seal=="headcas" and nst==DRAIN) else 0
                nseq,nns=(ns,ns+1) if seqat=="open" else (seq,ns)
                T=(sb(bufs,b,(nst,0,nbit,0,nseq,())),ws,sq,inf,nns,wire,srs,dl)
                if lostS: rec("LOST",T,f"recycle b{b} drops late reservations")
                if fl>0 and not msgs: pass
                out.append((f"PT recycle b{b} -> {nst}"+(f" seq={nseq}" if seqat=="open" else ""),T))
            if st==OPEN and bit==0 and dl>0 and hd+1<=cap and b not in sq:
                nb=(st,hd+1,bit,fl+1,seq,msgs+((-1,dl,hd,1,True),))
                out.append((f"PT reserve-reply-in-OPEN b{b}",(sb(bufs,b,nb),ws,sq,inf,ns,wire,srs,dl-1)))
            if st==DRAIN:
                if dl>0 and hd+1<=cap:
                    nb=(st,hd+1,bit,fl+1,seq,msgs+((-1,dl,hd,1,True),))
                    out.append((f"PT drain-reserve b{b}",(sb(bufs,b,nb),ws,sq,inf,ns,wire,srs,dl-1)))
                if openrule in ("any","cqe") or ost not in (OPEN,DRAIN):
                    out.append((f"PT drained b{b} -> OPEN",(sb(bufs,b,(OPEN,hd,0,fl,seq,msgs)),ws,sq,inf,ns,wire,srs,dl)))
        if inf is None and sq:
            b=sq[0]; st,hd,bit,fl,seq,msgs=bufs[b]
            if hd==fl:
                if seal=="recheck":
                    out.append((f"PT submit-check b{b} snap={hd}",(bufs,ws,sq,("chk",b,hd),ns,wire,srs,dl)))
                else:
                    out.append((f"PT submit b{b} len={hd}",(sb(bufs,b,(SENDING,hd,bit,fl,seq,msgs)),ws,sq[1:],("snd",b,hd),ns,wire,srs,dl)))
        if inf is not None and inf[0]=="chk":
            _,b,snap=inf; st,hd,bit,fl,seq,msgs=bufs[b]
            if hd==snap and fl==hd:
                out.append((f"PT recheck ok b{b} submit len={snap}",(sb(bufs,b,(SENDING,hd,bit,fl,seq,msgs)),ws,sq[1:],("snd",b,snap),ns,wire,srs,dl)))
            else:
                out.append((f"PT recheck fail b{b}, retry later",(bufs,ws,sq,None,ns,wire,srs,dl)))
        if inf is not None and inf[0]=="snd":
            _,b,snap=inf; st,hd,bit,fl,seq,msgs=bufs[b]; nw=wire; bad=[]
            for m in sorted(msgs,key=lambda m:m[2]):
                if m[2]+m[3]<=snap:
                    if not m[4]: bad.append("TORN")
                    nw=nw+((m[0],m[1],seq),)
                else: bad.append("LOST")
            T=(sb(bufs,b,(FREE,hd,bit,fl,seq,())),ws,sq,None,ns,nw,srs,dl)
            o=T[0][1-b]
            if openrule=="cqe" and o[0]==OPEN and o[1]>0 and (1-b) not in T[2]:
                nseq,nns=(ns,ns+1) if seqat=="seal" else (o[4],ns)
                T=(sb(T[0],1-b,(SEALED,o[1],1 if seal=="headcas" else o[2],o[3],nseq,o[5])),ws,sq+(1-b,),None,nns,nw,srs,dl)
            for k in bad: rec(k,T,"at complete")
            out.append((f"PT complete b{b} seq={seq}",T))
        # wire checks
        last={}; ls=-1
        for (w,i,sg) in wire:
            if w>=0:
                if i<last.get(w,-1): rec("ORDER",S,"")
                last[w]=max(last.get(w,-1),i)
            if sg<ls: rec("SEQ",S,"")
            ls=max(ls,sg)
        sent={(w,i):sg for (w,i,sg) in wire if w>=0}
        for (w,i,sr) in srs:
            if (w,i) in sent and sent[(w,i)]!=sr: rec("STALESEQ",S,"")
        edges[S]=[T for _,T in out]
        for lab,T in out:
            if T not in seen: seen[T]=(S,lab); q.append(T)
        if len(seen)>maxst: print("  LIMIT"); break
    # liveness: can every state reach 'all delivered'?
    done={S for S in edges if len(S[5])==total}
    rev={}
    for S,ts in edges.items():
        for T in ts: rev.setdefault(T,[]).append(S)
    good=set(done); dq=deque(done)
    while dq:
        T=dq.popleft()
        for P in rev.get(T,[]):
            if P not in good: good.add(P); dq.append(P)
    bad=[S for S in edges if S not in good]
    if bad: rec("NOLIVE",min(bad,key=lambda s:len(tr(seen,s))),"completion unreachable")
    return V,seen
def tr(seen,S):
    o=[]
    while seen[S] is not None: P,l=seen[S]; o.append(l); S=P
    return o[::-1]
if __name__=="__main__":
    seal,openrule,drain,seqat=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4]
    wfail=sys.argv[5] if len(sys.argv)>5 else "other"
    show=sys.argv[6].split(",") if len(sys.argv)>6 else []
    cfgs=[([[1,2]],2,0),([[1,2,1]],2,0),([[1,1],[1]],2,0),([[1,2]],2,1),([[1,1],[1]],2,1),([[2,1],[1,1]],3,1)]
    allv={}
    for sz,cap,R in cfgs:
        V,seen=run(seal,openrule,drain,seqat,sz,cap,R,wfail)
        print(f"  {seal}/{openrule}/drain={drain}/seq@{seqat}/fail={wfail} writers={sz} cap={cap} replies={R} states={len(seen)} viol={sorted(V)}")
        for k,(S,m) in V.items():
            if k not in allv or len(tr(seen,S))<len(allv[k][1]): allv[k]=(m,tr(seen,S),sz,cap,R)
    for k in show:
        if k in allv:
            m,t,sz,cap,R=allv[k]; print(f"  -- {k} ({m}) writers={sz} cap={cap} R={R}:"); [print("      ",x) for x in t]
