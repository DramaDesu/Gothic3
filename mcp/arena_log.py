import socket, json, time, csv
s=socket.create_connection(("127.0.0.1",5556), timeout=30)
def call(obj, timeout=30):
    s.settimeout(timeout)
    s.sendall((json.dumps(obj)+"\n").encode())
    buf=b""
    while b"\n" not in buf:
        c=s.recv(262144)
        if not c: break
        buf+=c
    return json.loads(buf.decode().strip() or "{}")

rows=[]
t0=time.time()
last_near=0.0
near={}
while time.time()-t0 < 75:
    now=time.time()-t0
    try:
        p=call({"cmd":"combat_state"})
    except Exception as e:
        print("sample failed:", type(e).__name__, flush=True); break
    if not p.get("ok"):
        time.sleep(0.1); continue
    e=p["entity"]
    if now-last_near > 1.0:
        try:
            n=call({"cmd":"nearby_npcs","radius":900})
            near={x["state"].get("name"): x for x in (n.get("npcs") or [])}
            last_near=now
        except Exception: pass
    enemies=[v for k,v in near.items() if v["state"].get("attitude_to_player")==4]
    rows.append({
        "t": round(now,2),
        "hp": e.get("hitpoints"),
        "cs": e.get("combat_state"),
        "attacker": e.get("current_attacker",""),
        "last_hit_ts": e.get("last_hit_timestamp"),
        "enemies_near": len(enemies),
        "nearest_enemy_d": round(min([x["distance"] for x in enemies], default=-1)),
        "enemy_cs": ";".join(str(x["state"].get("combat_state")) for x in enemies[:4]),
    })
    time.sleep(0.1)
s.close()
with open("arena_log.csv","w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
print(f"samples={len(rows)} duration={rows[-1]['t']}s hp {rows[0]['hp']} -> {rows[-1]['hp']}", flush=True)
print("arena log done", flush=True)
