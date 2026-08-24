import socket, json, time, csv, importlib.util
spec=importlib.util.spec_from_file_location("g3", r"F:\Projects\Gothic3\mcp\g3_mcp.py")
g3=importlib.util.module_from_spec(spec); spec.loader.exec_module(g3)
s=None
def connect():
    global s
    s=socket.create_connection(("127.0.0.1",5556), timeout=90)
def call(o, timeout=90):
    s.settimeout(timeout)
    s.sendall((json.dumps(o)+"\n").encode())
    buf=b""
    while b"\n" not in buf:
        c=s.recv(262144)
        if not c: break
        buf+=c
    return json.loads(buf.decode().strip() or "{}")

print("launch:", g3.tool_launch({"width":1600,"height":900}).get("ok"), flush=True)
connect()
print("load arena_fresh:", call({"cmd":"load_save","name":"arena_fresh"}, timeout=20), flush=True)
t0=time.time()
while time.time()-t0 < 180:
    time.sleep(4)
    try:
        if call({"cmd":"ping"}, timeout=15).get("state")=="INGAME": break
    except Exception:
        try: connect()
        except Exception: pass
print(f"INGAME after {time.time()-t0:.0f}s", flush=True)
time.sleep(3)
print("tips off:", call({"cmd":"tips","hide":1}), flush=True)
# dismiss whatever popup is on screen and move away from the scripted start scene
g3.tool_input({"keys":["return"],"delay":0.8})
g3.tool_input({"keys":["escape"],"delay":0.8})
p=call({"cmd":"combat_state"})["entity"]
x,y,z=p["position"]
print("player at", p["position"], "hp", p["hitpoints"], flush=True)
print("teleport aside:", call({"cmd":"teleport","x":x+4000,"y":y+200,"z":z+4000}), flush=True)
time.sleep(3)
print("spawn 2 orcs:", json.dumps(call({"cmd":"spawn","template":"Orc_Warrior_01","count":2,"distance":500}), ensure_ascii=False)[:220], flush=True)
time.sleep(2)

rows=[]; t0=time.time(); last_near=0; near={}
while time.time()-t0 < 70:
    now=time.time()-t0
    try:
        pr=call({"cmd":"combat_state"}, timeout=20)
    except Exception:
        break
    if not pr.get("ok"):
        time.sleep(0.1); continue
    e=pr["entity"]
    if now-last_near > 1.5:
        try:
            n=call({"cmd":"nearby_npcs","radius":900}, timeout=40)
            near={x2["state"].get("name")+str(i): x2 for i,x2 in enumerate(n.get("npcs") or [])}
            last_near=now
        except Exception: pass
    foes=[v for v in near.values() if v["state"].get("attitude_to_player")==4]
    rows.append({"t":round(now,2), "hp":e.get("hitpoints"), "action":e.get("action"),
                 "ani_state":e.get("ani_state"), "ani_phase":e.get("ani_phase"),
                 "state_time":e.get("state_time"), "task":e.get("task",""),
                 "attacker":e.get("current_attacker",""), "foes":len(foes),
                 "foe_d":round(min([f["distance"] for f in foes], default=-1)),
                 "foe_actions":";".join(str(f["state"].get("action")) for f in foes[:3])})
    time.sleep(0.1)
with open("arena2.csv","w",newline="",encoding="utf-8") as f:
    w=csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
print(f"samples={len(rows)} hp {rows[0]['hp']} -> {rows[-1]['hp']}", flush=True)
print("arena run done", flush=True)
