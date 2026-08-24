import socket, json, time, importlib.util
spec=importlib.util.spec_from_file_location("g3", r"F:\Projects\Gothic3\mcp\g3_mcp.py")
g3=importlib.util.module_from_spec(spec); spec.loader.exec_module(g3)
def call(obj, timeout=60):
    s=socket.create_connection(("127.0.0.1",5556), timeout=timeout)
    try:
        s.sendall((json.dumps(obj)+"\n").encode()); s.settimeout(timeout)
        buf=b""
        while b"\n" not in buf:
            c=s.recv(65536)
            if not c: break
            buf+=c
        return json.loads(buf.decode().strip() or "{}")
    finally: s.close()

print("new_game:", call({"cmd":"new_game"}, timeout=15), flush=True)
t0=time.time()
while time.time()-t0 < 700:
    time.sleep(6)
    try:
        if call({"cmd":"ping"}, timeout=8).get("game_running"): break
    except Exception: pass
print(f"session up after {time.time()-t0:.0f}s", flush=True)
# the new game leaves the menu open; close it the way the engine does
time.sleep(5)
for attempt in range(20):
    time.sleep(6)
    try:
        p=call({"cmd":"combat_state"}, timeout=10)
    except Exception:
        continue
    if p.get("ok"):
        e=p["entity"]
        print(f"fresh player: lvl={e.get('level')} hp={e.get('hitpoints')}/{e.get('hitpoints_max')} "
              f"str={e.get('strength')} dex={e.get('dexterity')} pos={e.get('position')}", flush=True)
        if e.get("hitpoints", 0) > 1:
            print("save:", call({"cmd":"save_game","name":"arena_fresh"}), flush=True)
            time.sleep(10)
            print("ping:", call({"cmd":"ping"}), flush=True)
            print("saves:", json.dumps(call({"cmd":"list_saves"}), ensure_ascii=False)[-200:], flush=True)
            break
print("fresh done", flush=True)
