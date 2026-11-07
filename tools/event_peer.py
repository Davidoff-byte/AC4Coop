"""P2 event layer - second cut. Sender retransmits on its own timer; receiver acks every packet
including duplicates; the pump uses select() so neither side starves."""
from __future__ import annotations
import argparse, random, select, socket, struct, sys, time

MAGIC=b"AC4V"; VER=1
HDR=struct.Struct("<4sBBIQIIIH")
FLAG_ACK_ONLY=1
RETX_MS=50
MAX_RETX=200

def now_ms(): return int(time.monotonic()*1000)

class Sender:
    def __init__(self, sock, peer, loss=0.0, jitter_ms=0):
        self.sock=sock; self.peer=peer; self.loss=loss; self.jitter=jitter_ms
        self.seq=0; self.pending={}; self.sent=0; self.retx=0; self.acked=set(); self.first={}
    def _emit(self, pkt):
        if self.loss and random.random()<self.loss: return
        if self.jitter: time.sleep(random.uniform(0,self.jitter)/1000.0)
        try: self.sock.sendto(pkt, self.peer)
        except OSError: pass
    def push(self, ev_id, kind, payload=b""):
        self.seq+=1
        pkt=HDR.pack(MAGIC,VER,0,self.seq,ev_id,kind,0,0,len(payload))+payload
        self.pending[self.seq]=[pkt,0,0,ev_id]; self.sent+=1; self.first[ev_id]=now_ms()
        self._emit(pkt)
    def on_ack(self, ack, bits):
        for seq in list(self.pending):
            if seq<=ack or (seq<=ack+32 and (bits>>(seq-ack-1))&1):
                self.acked.add(self.pending[seq][3]); del self.pending[seq]
    def tick(self):
        t=now_ms()
        for seq,rec in list(self.pending.items()):
            pkt,last,tries,ev=rec
            if tries>=MAX_RETX: continue
            if t-last>=RETX_MS:
                self._emit(pkt); rec[1]=t; rec[2]=tries+1; self.retx+=1

class Receiver:
    def __init__(self, sock):
        self.sock=sock; self.next_seq=1; self.bits=0; self.seen=set(); self.received=set()
        self.delivered=[]; self.dups=0
    def _ack(self):
        return HDR.pack(MAGIC,VER,FLAG_ACK_ONLY,self.next_seq,0,0,self.next_seq-1,self.bits,0)
    def handle(self,data,addr,drop=0.0,jitter_ms=0):
        if len(data)<HDR.size: return
        magic,ver,flags,seq,ev_id,kind,ack,bits,plen=HDR.unpack_from(data,0)
        if magic!=MAGIC: return
        if flags & FLAG_ACK_ONLY: return
        if seq>0: self.received.add(seq)
        while self.next_seq in self.received:
            self.received.discard(self.next_seq); self.next_seq+=1
        self.bits=0
        for i in range(32):
            if (self.next_seq+i) in self.received: self.bits |= 1<<i
        if ev_id in self.seen: self.dups+=1
        else: self.seen.add(ev_id); self.delivered.append(ev_id)
        if drop and random.random()<drop: return
        if jitter_ms: time.sleep(random.uniform(0,jitter_ms)/1000.0)
        try: self.sock.sendto(self._ack(),addr)
        except OSError: pass

def selftest(argv):
    n=200; loss=0.30; jitter=40
    for i,a in enumerate(argv):
        if a=="--n": n=int(argv[i+1])
        elif a=="--loss": loss=float(argv[i+1])
        elif a=="--jitter": jitter=int(argv[i+1])
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); s.setblocking(False)
    r=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); r.bind(("127.0.0.1",0)); r.setblocking(False)
    tx=Sender(s,r.getsockname(),loss=loss,jitter_ms=jitter); rx=Receiver(r)
    t0=now_ms(); pushed=0
    deadline=t0+120000
    while now_ms()<deadline:
        t=now_ms()
        while pushed<n and t-t0>=pushed*5:
            pushed+=1; tx.push(0xA000+pushed,1,b"\x01\x02")
        tx.tick()
        rd,_,_=select.select([s,r],[],[],0.002)
        for sock in rd:
            while True:
                try: data,addr=sock.recvfrom(2048)
                except BlockingIOError: break
                if sock is s:
                    _,_,_,_,_,_,ack,bits,_=HDR.unpack_from(data,0); tx.on_ack(ack,bits)
                else:
                    rx.handle(data,addr,drop=loss,jitter_ms=jitter)
        if pushed>=n and not tx.pending: break
    el=now_ms()-t0
    ok=len(rx.delivered)==n and len(set(rx.delivered))==n
    print(f"pushed {n}  delivered {len(rx.delivered)}  dups {rx.dups}  retx {tx.retx}  elapsed {el} ms  loss {loss*100:.0f}% jitter {jitter}ms")
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__=="__main__":
    sys.exit(selftest(sys.argv[1:]))

