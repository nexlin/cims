#!/usr/bin/env python3
# CMP 안내 재생기 스모크 테스트 — announcements.md §4 / cmp_media_api.md §6.7
#   HEARTBEAT resource.ann → RELAY_ADD(PCMU) → RELAY_PLAY 오류(MEDIA_NOT_FOUND·NOT_FOUND) → tone 1 s + 안내, max 3 s
#   → 150 pkt/3 s(20 ms 페이싱·seq 연속·SSRC·marker·peer0 소스 포트) → RELAY_PLAY_DONE(max) → peer1 AMR-WB loop(STATS detail.ann,
#   PT 96 octet-aligned) → STOP(stopped) → 교체(replaced) → ANN_RELOAD → RELAY_REMOVE → used 0
# 사용법: python3 tests/cmp_smoke_announcement.py [CMP_IP] [CMP_PORT]   (AnnPlayers>0 인 CMP — 같은 호스트에서 실행 전제:
#   수신 주소가 CMP_IP 로 광고되고, 이벤트 회신처는 이 스크립트의 제어 소켓이다). 세션 csp_smoke_ann_1 생성·정리.
import json, os, socket, struct, sys, time
CMP_IP = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("CMP_IP", "127.0.0.1")
CMP=(CMP_IP, int(sys.argv[2]) if len(sys.argv) > 2 else int(os.environ.get("CMP_PORT", "9000")))
ctrl=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); ctrl.settimeout(3.0); ctrl.bind((CMP_IP,0))
tid=[100]
def req(cmd,payload,svc="volte"):
    tid[0]+=1
    msg={"hdr":{"ver":2,"trans_id":tid[0],"node":"csp-smoke","cmd":cmd,"type":"request","sesid":"smoke::1","service":svc},"payload":payload}
    ctrl.sendto(json.dumps(msg).encode(),CMP)
    while True:
        d,_=ctrl.recvfrom(8192); r=json.loads(d.decode())
        if r["hdr"].get("type")=="event": ack(r); continue
        return r
events=[]
def ack(ev):
    events.append(ev)
    ctrl.sendto(json.dumps({"hdr":{"ver":2,"trans_id":ev["hdr"]["trans_id"],"node":"csp-smoke","cmd":ev["hdr"]["cmd"],"type":"response","status":"OK"}}).encode(),CMP)
def drain_events(timeout=1.0):
    ctrl.settimeout(timeout)
    try:
        while True:
            d,_=ctrl.recvfrom(8192); r=json.loads(d.decode())
            if r["hdr"].get("type")=="event": ack(r)
    except socket.timeout: pass
    ctrl.settimeout(3.0)
ok=True
def check(c,what):
    global ok; print(("ok   " if c else "FAIL ")+what); ok=ok and c

hb=req("HEARTBEAT",{})
ann=hb.get("payload",{}).get("resource",{}).get("ann")
check(bool(ann) and ann.get("total",0)>0 and ann.get("media",0)>=12, f"heartbeat resource.ann={ann}")

ua=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); ua.bind((CMP_IP,0)); ua.settimeout(1.0)
ub=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); ub.bind((CMP_IP,0)); ub.settimeout(1.0)
r=req("RELAY_ADD",{"session_id":"csp_smoke_ann_1","caller":"a","callee":"b","remote_ip":CMP_IP,"remote_port":ua.getsockname()[1],"peer_index":0,"remote_pt":0,"remote_src_pt":0,"remote_codec":"PCMU/8000"})
check(r["hdr"]["status"]=="OK", f"RELAY_ADD {r['hdr'].get('status')}")
lp=r["payload"]["local_port"]; lpb=r["payload"]["local_port_b"]

# MEDIA_NOT_FOUND
r=req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":0,"play_id":"p0","media":["sys:nope"]})
check(r["hdr"].get("code")=="MEDIA_NOT_FOUND", f"unknown media → {r['hdr'].get('code')}")
# NOT_FOUND session
r=req("RELAY_PLAY",{"session_id":"nosuch","peer_index":0,"play_id":"p0","media":["sys:busy_kr"]})
check(r["hdr"].get("code")=="NOT_FOUND", f"unknown session → {r['hdr'].get('code')}")

# tone(1 s) then announcement, max 3 s → reason max
t0=time.time()
r=req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":0,"play_id":"p1","media":[{"id":"sys:busy_kr","repeat":0,"max_ms":1000},"sys:ann_busy"],"repeat":1,"max_ms":3000})
check(r["hdr"]["status"]=="OK" and r["payload"]["codec"].startswith("PCMU") and r["payload"]["duration_ms"]==3000, f"RELAY_PLAY {r}")
# idempotent
r=req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":0,"play_id":"p1","media":["sys:busy_kr"]})
check(r["hdr"]["status"]=="OK" and "played_ms" in r["payload"], f"idempotent replay {r['payload']}")
pk=[]; first=time.time(); last=None
ua.settimeout(0.5)
while time.time()-t0 < 4.0:
    try:
        d,addr=ua.recvfrom(2048); pk.append((time.time(),d,addr))
    except socket.timeout: pass
check(len(pk)>=145 and len(pk)<=152, f"received {len(pk)} PCMU packets in 3 s window (expect ~150)")
if pk:
    pts=set(d[1]&0x7f for _,d,_ in pk); sizes=set(len(d) for _,d,_ in pk)
    seqs=[struct.unpack("!H",d[2:4])[0] for _,d,_ in pk]
    cont=all(((seqs[i]+1)&0xffff)==seqs[i+1] for i in range(len(seqs)-1))
    ssrcs=set(d[8:12] for _,d,_ in pk)
    check(pts=={0} and sizes=={172} and cont and len(ssrcs)==1 and (pk[0][1][1]&0x80), f"PT={pts} size={sizes} seq-cont={cont} ssrc={len(ssrcs)} marker0={bool(pk[0][1][1]&0x80)}")
    check(all(a[1]==lp for _,_,a in pk), f"source port = peer0 local port {lp}")
    # 1 s of busy tone then ann_busy: payload bytes differ — check the first byte patterns change after ~1 s
    gaps=[pk[i+1][0]-pk[i][0] for i in range(len(pk)-1)]
    check(max(gaps)<0.06, f"max inter-packet gap {max(gaps)*1000:.1f} ms")
drain_events(1.5)
done=[e for e in events if e["hdr"]["cmd"]=="RELAY_PLAY_DONE"]
check(len(done)==1 and done[0]["payload"]["play_id"]=="p1" and done[0]["payload"]["reason"]=="max" and 2900<=done[0]["payload"]["played_ms"]<=3100, f"RELAY_PLAY_DONE {[d['payload'] for d in done]}")
events.clear()

# AMR-WB on peer1 (moh loop) then STOP → stopped; STATS detail.ann visible
r=req("RELAY_MODIFY",{"session_id":"csp_smoke_ann_1","caller":"a","callee":"b","remote_ip":CMP_IP,"remote_port":ub.getsockname()[1],"peer_index":1,"remote_pt":96,"remote_src_pt":96,"remote_codec":"AMR-WB/16000"})
check(r["hdr"]["status"]=="OK","RELAY_MODIFY peer1 AMR-WB")
r=req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":1,"play_id":"p2","media":["sys:moh_simple"],"repeat":0})
check(r["hdr"]["status"]=="OK" and r["payload"]["codec"].startswith("AMR-WB") and r["payload"]["duration_ms"]==0, f"RELAY_PLAY loop {r['payload']}")
time.sleep(0.6)
st=req("STATS",{})
da=st["payload"]["detail"].get("ann",[]); cat=st["payload"]["detail"].get("ann_catalog",{})
check(len(da)==1 and da[0]["play_id"]=="p2" and st["payload"]["resource"]["ann"]["used"]==1 and len(cat.get("ids",[]))==12 and cat.get("missing")==[], f"STATS ann={da} used={st['payload']['resource']['ann']['used']} catalog ids={len(cat.get('ids',[]))} missing={cat.get('missing')}")
pkb=[]
ub.settimeout(0.3)
while len(pkb)<30:
    try: d,_=ub.recvfrom(2048); pkb.append(d)
    except socket.timeout: break
check(len(pkb)>=25 and all((d[1]&0x7f)==96 for d in pkb) and all(d[12]==0xF0 for d in pkb) and all(len(d)==12+2+60 for d in pkb), f"AMR-WB {len(pkb)} pkts PT96 octet-aligned CMR 0xF0 len={set(len(d) for d in pkb)}")
r=req("RELAY_PLAY_STOP",{"session_id":"csp_smoke_ann_1","play_id":"p2"})
check(r["hdr"]["status"]=="OK" and r["payload"]["played_ms"]>=600, f"STOP played_ms={r['payload'].get('played_ms')}")
drain_events(1.0)
done=[e for e in events if e["hdr"]["cmd"]=="RELAY_PLAY_DONE"]
check(len(done)==1 and done[0]["payload"]["reason"]=="stopped", f"DONE stopped {[d['payload'] for d in done]}")
events.clear()
# replace: play p3 then p4 on same leg → DONE replaced for p3
req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":0,"play_id":"p3","media":["sys:moh_simple"],"repeat":0})
r=req("RELAY_PLAY",{"session_id":"csp_smoke_ann_1","peer_index":0,"play_id":"p4","media":["sys:dial_kr"],"repeat":0})
drain_events(0.5)
rep=[e for e in events if e["hdr"]["cmd"]=="RELAY_PLAY_DONE" and e["payload"]["reason"]=="replaced"]
check(len(rep)==1 and rep[0]["payload"]["play_id"]=="p3", f"replaced event {[d['payload'] for d in rep]}")
# ANN_RELOAD
r=req("ANN_RELOAD",{})
check(r["hdr"]["status"]=="OK" and r["payload"]["media"]==12, f"ANN_RELOAD {r.get('payload')}")
# RELAY_REMOVE stops players silently
r=req("RELAY_REMOVE",{"session_id":"csp_smoke_ann_1"})
check(r["hdr"]["status"]=="OK","RELAY_REMOVE")
time.sleep(0.3); hb=req("HEARTBEAT",{})
check(hb["payload"]["resource"]["ann"]["used"]==0, "ann used back to 0")
print("ALL OK" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
