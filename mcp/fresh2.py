import socket, json, time, importlib.util
spec=importlib.util.spec_from_file_location("g3", r"F:\Projects\Gothic3\mcp\g3_mcp.py")
g3=importlib.util.module_from_spec(spec); spec.loader.exec_module(g3)
def call(obj, timeout=90):
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
print("launch:", g3.tool_launch({"width":1600,"height":900}).get("ok"), flush=True)
print("new_game:", call({"cmd":"new_game"}, timeout=15), flush=True)
t0=time.time()
while time.time()-t0 < 700:
    time.sleep(6)
    try:
        st=call({"cmd":"ping"}, timeout=8)
        if st.get("game_running") and st.get("state")=="INGAME": break
    except Exception: pass
print(f"INGAME after {time.time()-t0:.0f}s | {call({'cmd':'ping'})}", flush=True)
for i in range(15):
    time.sleep(5)
    p=call({"cmd":"combat_state"})
    if p.get("ok") and p["entity"].get("hitpoints",0)>1:
        e=p["entity"]
        print(f"fresh: hp={e['hitpoints']}/{e['hitpoints_max']} str={e['strength']} dex={e['dexterity']} pos={e['position']}", flush=True)
        break
print("saves before:", json.dumps(call({"cmd":"list_saves"}), ensure_ascii=False)[:150], flush=True)
print("save:", call({"cmd":"save_game","name":"arena_fresh"}), flush=True)
time.sleep(12)
print("ping:", call({"cmd":"ping"}), flush=True)
print("saves after:", json.dumps(call({"cmd":"list_saves"}), ensure_ascii=False)[-160:], flush=True)
print("fresh2 done", flush=True)
