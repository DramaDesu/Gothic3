import socket, json, time, importlib.util, sys
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

print("launch:", g3.tool_launch({"width":1600,"height":900}), flush=True)
print("attack_speed default:", call({"cmd":"attack_speed"}, timeout=15), flush=True)
print("new_game:", call({"cmd":"new_game"}, timeout=15), flush=True)
t0=time.time()
while time.time()-t0 < 600:
    time.sleep(6)
    try:
        if call({"cmd":"ping"}, timeout=8).get("game_running"): break
    except Exception: pass
print(f"INGAME after {time.time()-t0:.0f}s", flush=True)
g3.tool_input({"keys":["escape"],"delay":1.0})
print("quickload:", g3.tool_input({"keys":["f9"],"delay":2.0}), flush=True)
t0=time.time()
while time.time()-t0 < 300:
    time.sleep(8)
    try: p=call({"cmd":"combat_state"}, timeout=10)
    except Exception: continue
    if p.get("ok") and p["entity"].get("hitpoints",0) > 1:
        e=p["entity"]; print(f"player: lvl={e['level']} hp={e['hitpoints']} str={e['strength']}", flush=True); break
print("set attack_speed 1.6:", call({"cmd":"attack_speed","value":1.6}), flush=True)
print("read back:", call({"cmd":"attack_speed"}), flush=True)
print("cycle2 done", flush=True)
