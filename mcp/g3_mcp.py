#!/usr/bin/env python3
"""MCP server for driving a live Gothic 3 (CP 1.75.14 + PUP) instance.

Transport to the game: Script_RemoteControl.dll (ZeroMQ + protobuf, tcp://127.0.0.1:5555).
Process lifecycle, window capture and log access are handled directly via WinAPI.

Registered as a stdio MCP server; tools:
  g3_launch, g3_status, g3_entity, g3_goto, g3_screenshot, g3_logs, g3_quit
"""
import base64
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import struct
import subprocess
import sys
import time

GAME_DIR = r"F:\SteamLibrary\steamapps\common\Gothic 3"
GAME_EXE = os.path.join(GAME_DIR, "Gothic3.exe")
DOC_DIR = os.path.expandvars(r"%USERPROFILE%\Documents\gothic3")
USER_OPTIONS = os.path.join(DOC_DIR, "UserOptions.ini")
PARU_INI = os.path.join(GAME_DIR, "ini", "paru.ini")
SHOT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
RC_ENDPOINT = "tcp://127.0.0.1:5555"

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
gdi32 = ctypes.windll.gdi32

kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]

# ---------------------------------------------------------------------------
# proto2 hand-rolled encoding (g3rc.proto, verified against generated pb code)
# ---------------------------------------------------------------------------

def _varint(n: int) -> bytes:
    out = b""
    while True:
        b7 = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b7 | 0x80])
        else:
            out += bytes([b7])
            return out


def _tag(field: int, wire: int) -> bytes:
    return _varint((field << 3) | wire)


def _len_field(field: int, payload: bytes) -> bytes:
    return _tag(field, 2) + _varint(len(payload)) + payload


def _str_field(field: int, s: str) -> bytes:
    return _len_field(field, s.encode("utf-8"))


def _float_field(field: int, f: float) -> bytes:
    return _tag(field, 5) + struct.pack("<f", f)


def _bool_field(field: int, v: bool) -> bytes:
    return _tag(field, 0) + _varint(1 if v else 0)


def _vector(x, y, z) -> bytes:
    return _float_field(1, x) + _float_field(2, y) + _float_field(3, z)


def _position(translation, scale=(1.0, 1.0, 1.0), rotation=(0.0, 0.0, 0.0)) -> bytes:
    # Position{required Vector translation=1, scale=2, required EulerAngles rotation=3}
    return (_len_field(1, _vector(*translation))
            + _len_field(2, _vector(*scale))
            + _len_field(3, _vector(*rotation)))


def _decode(buf: bytes):
    """Decode one proto message into {field: [(wire, value)]}. Nested messages stay bytes."""
    fields = {}
    i = 0
    while i < len(buf):
        key = 0
        shift = 0
        while True:
            b = buf[i]
            i += 1
            key |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        field, wire = key >> 3, key & 7
        if wire == 0:  # varint
            val = 0
            shift = 0
            while True:
                b = buf[i]
                i += 1
                val |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
        elif wire == 2:  # length-delimited
            ln = 0
            shift = 0
            while True:
                b = buf[i]
                i += 1
                ln |= (b & 0x7F) << shift
                shift += 7
                if not b & 0x80:
                    break
            val = buf[i:i + ln]
            i += ln
        elif wire == 5:  # 32-bit
            val = struct.unpack("<f", buf[i:i + 4])[0]
            i += 4
        elif wire == 1:  # 64-bit
            val = struct.unpack("<d", buf[i:i + 8])[0]
            i += 8
        else:
            raise ValueError(f"wire type {wire} unsupported")
        fields.setdefault(field, []).append((wire, val))
    return fields


def _first(fields, n, default=None):
    v = fields.get(n)
    return v[0][1] if v else default


# RequestContainer fields: 1=request_number, oneof: 2=heartbeat, 3=entity, 4=goto,
#                          5=spawn, 6=property  (root g3rc.proto is STALE; truth = generated pb / g3dit proto)
# ResponseContainer fields: 1=request_number, 2=status (0 FAILED/1 SUCCESSFUL), 3=message (German),
#   oneof: 4=heartbeat_response{1=status}, 5=entity_response{1=Position,2=name,3=guid}, 6=void, 7=property_response
HEARTBEAT_STATUS = {0: "MENU", 1: "LOADING", 2: "INGAME"}  # LOADING is dead code, never sent

_req_counter = int(time.time()) % 100000


def _request(body_field: int, body: bytes) -> bytes:
    global _req_counter
    _req_counter += 1
    return _tag(1, 0) + _varint(_req_counter) + _len_field(body_field, body)


# ---------------------------------------------------------------------------
# ZeroMQ transport
# ---------------------------------------------------------------------------

def rc_roundtrip(payload: bytes, timeout_ms: int = 4000) -> bytes:
    import zmq
    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.DEALER)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(RC_ENDPOINT)
    try:
        sock.send(payload)
        if not sock.poll(timeout_ms):
            raise TimeoutError(f"no reply from {RC_ENDPOINT} in {timeout_ms} ms "
                               "(game not running, still booting, or Script_RemoteControl.dll not loaded)")
        return sock.recv()
    finally:
        sock.close()


def rc_call(body_field: int, body: bytes, timeout_ms: int = 4000):
    raw = rc_roundtrip(_request(body_field, body), timeout_ms)
    resp = _decode(raw)
    # The server answers unknown requests with a structurally invalid container
    # (required status missing) — tolerate that instead of crashing.
    status = _first(resp, 2, None)
    message = _first(resp, 3, b"")
    if isinstance(message, bytes):
        message = message.decode("utf-8", "replace")
    if status is None:
        message = message or "server sent no status (unknown request type?)"
        status = 0
    return resp, status, message


def _decode_entity_response(resp):
    ent = _first(resp, 5)
    out = {}
    if ent is None:
        return out
    ef = _decode(ent)
    name = _first(ef, 2)
    guid = _first(ef, 3)
    if isinstance(name, bytes):
        out["name"] = name.decode("utf-8", "replace")
    if isinstance(guid, bytes):
        out["guid"] = guid.decode("utf-8", "replace")
    pos = _first(ef, 1)
    if pos is not None:
        pf = _decode(pos)

        def vec(field):
            raw = _first(pf, field)
            if raw is None:
                return None
            vf = _decode(raw)
            return [_first(vf, 1), _first(vf, 2), _first(vf, 3)]

        out["translation"] = vec(1)
        out["scale"] = vec(2)
        out["rotation_ypr"] = vec(3)
    return out


# ---------------------------------------------------------------------------
# process / window helpers
# ---------------------------------------------------------------------------

def game_pid():
    out = subprocess.run(["tasklist", "/fi", "imagename eq Gothic3.exe", "/fo", "csv"],
                         capture_output=True).stdout.decode("cp866", "replace")
    m = re.search(r'"Gothic3\.exe","(\d+)"', out)
    return int(m.group(1)) if m else None


def game_hwnd(pid):
    result = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, lp):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            ln = user32.GetWindowTextLengthW(hwnd)
            if ln:
                result.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return result[0] if result else None


def module_loaded(pid, name):
    h = kernel32.OpenProcess(0x0410, False, pid)
    if not h:
        return False
    try:
        psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                               wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
        psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
        arr = (ctypes.c_void_p * 4096)()
        needed = wt.DWORD()
        if not psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(needed), 0x03):
            return False
        buf = ctypes.create_unicode_buffer(512)
        for i in range(min(needed.value // ctypes.sizeof(ctypes.c_void_p), 4096)):
            psapi.GetModuleFileNameExW(h, arr[i], buf, 512)
            if os.path.basename(buf.value).lower() == name.lower():
                return True
        return False
    finally:
        kernel32.CloseHandle(h)


def edit_useroptions(width, height, window_mode):
    if not os.path.exists(USER_OPTIONS):
        return None
    src = open(USER_OPTIONS, encoding="ascii", errors="replace").read()
    out = re.sub(r"WindowMode=\d", f"WindowMode={window_mode}", src)
    out = re.sub(r"Resolution\.Width=\d+", f"Resolution.Width={width}", out)
    out = re.sub(r"Resolution\.Height=\d+", f"Resolution.Height={height}", out)
    open(USER_OPTIONS, "w", encoding="ascii", errors="replace").write(out)
    return src


def capture_window(hwnd, path):
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        raise RuntimeError("window has zero size (minimized?)")
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    # PW_RENDERFULLCONTENT (2) captures D3D content on Win8.1+
    if not user32.PrintWindow(hwnd, mem, 2):
        gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)

    class BMPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BMPINFOHEADER()
    bi.biSize = ctypes.sizeof(BMPINFOHEADER)
    bi.biWidth, bi.biHeight = w, -h
    bi.biPlanes, bi.biBitCount = 1, 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)

    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(path, "PNG")
    return path, (w, h)


# ---------------------------------------------------------------------------
# tools
# ---------------------------------------------------------------------------

UO_BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".useroptions.bak")


def tool_launch(args):
    if game_pid():
        return {"ok": True, "note": "already running", "pid": game_pid()}
    width = int(args.get("width", 1600))
    height = int(args.get("height", 900))
    window_mode = int(args.get("window_mode", 1))  # 0=fullscreen 1=windowed 2=borderless
    saved = edit_useroptions(width, height, window_mode)
    if saved is not None and not os.path.exists(UO_BACKUP):
        open(UO_BACKUP, "w", encoding="ascii", errors="replace").write(saved)
    p = subprocess.Popen([GAME_EXE], cwd=GAME_DIR)
    deadline = time.time() + float(args.get("boot_timeout", 90))
    rc_loaded = False
    while time.time() < deadline:
        time.sleep(2)
        if p.poll() is not None:
            return {"ok": False, "error": f"game exited during boot, code {p.poll()}"}
        if module_loaded(p.pid, "Script_RemoteControl.dll"):
            rc_loaded = True
            break
    status = None
    if rc_loaded:
        for _ in range(15):
            try:
                resp, st, msg = rc_call(2, b"", timeout_ms=2000)
                hb = _first(resp, 4)
                if hb is not None:
                    status = HEARTBEAT_STATUS.get(_first(_decode(hb), 1, -1), "?")
                break
            except TimeoutError:
                time.sleep(2)
    return {"ok": True, "pid": p.pid, "remote_control_loaded": rc_loaded, "heartbeat": status,
            "window": f"{width}x{height} mode={window_mode}"}


def tool_status(args):
    pid = game_pid()
    if not pid:
        return {"running": False}
    out = {"running": True, "pid": pid,
           "remote_control_loaded": module_loaded(pid, "Script_RemoteControl.dll")}
    if out["remote_control_loaded"]:
        try:
            resp, st, msg = rc_call(2, b"", timeout_ms=3000)
            hb = _first(resp, 4)
            out["heartbeat"] = HEARTBEAT_STATUS.get(_first(_decode(hb), 1, -1), "?") if hb is not None else None
        except TimeoutError as e:
            out["heartbeat_error"] = str(e)
    return out


def _entity_identifier(args):
    # EntityRequest oneof identifier: name=1, guid=2 (braced form), focus=3, editor=6
    if args.get("focus"):
        return _bool_field(3, True)
    if args.get("guid"):
        return _str_field(2, args["guid"])
    return _str_field(1, args.get("name", ""))


def tool_entity(args):
    resp, st, msg = rc_call(3, _entity_identifier(args))
    if st != 1:
        return {"ok": False, "error": msg or "request failed (no world loaded?)"}
    return {"ok": True, **_decode_entity_response(resp)}


def tool_move_entity(args):
    # Position fields are `required`, so read-modify-write: fetch current
    # scale/rotation first and only replace the translation.
    resp, st, msg = rc_call(3, _entity_identifier(args))
    if st != 1:
        return {"ok": False, "error": f"entity lookup failed: {msg}"}
    cur = _decode_entity_response(resp)
    x, y, z = args["position"]
    scale = cur.get("scale") or [1.0, 1.0, 1.0]
    rot = cur.get("rotation_ypr") or [0.0, 0.0, 0.0]
    body = (_entity_identifier(args)
            + _len_field(4, _position((x, y, z), scale, rot))
            + _bool_field(5, bool(args.get("put_to_ground", True))))
    resp, st, msg = rc_call(3, body, timeout_ms=8000)
    return {"ok": st == 1, "message": msg, **_decode_entity_response(resp)}


def tool_goto(args):
    if args.get("position"):
        x, y, z = args["position"]
        body = _len_field(3, _vector(x, y, z))
    elif args.get("guid"):
        body = _str_field(2, args["guid"])
    else:
        body = _str_field(1, args.get("name", ""))
    body += _bool_field(4, bool(args.get("put_to_ground", True)))
    resp, st, msg = rc_call(4, body, timeout_ms=8000)
    return {"ok": st == 1, "message": msg}


def tool_spawn(args):
    # SpawnRequest: template_name=1 | template_guid=2; location: entity_name=3 | entity_guid=4 | position=5
    if args.get("template_guid"):
        body = _str_field(2, args["template_guid"])
    else:
        body = _str_field(1, args.get("template_name", ""))
    if args.get("position"):
        x, y, z = args["position"]
        body += _len_field(5, _vector(x, y, z))
    elif args.get("at_entity_guid"):
        body += _str_field(4, args["at_entity_guid"])
    elif args.get("at_entity"):
        body += _str_field(3, args["at_entity"])
    resp, st, msg = rc_call(5, body, timeout_ms=8000)
    out = {"ok": st == 1, "message": msg}
    if st == 1:
        out.update(_decode_entity_response(resp))
    return out


def tool_property(args):
    # PropertyRequest: entity_name=1 | entity_guid=2; properties_get=3 (repeated PropertyIdentifier)
    if args.get("guid"):
        body = _str_field(2, args["guid"])
    else:
        body = _str_field(1, args.get("name", ""))
    idents = args.get("get") or [{}]
    for ident in idents:
        pi = b""
        if ident.get("property_set"):
            pi += _str_field(1, ident["property_set"])
        if ident.get("property"):
            pi += _str_field(2, ident["property"])
        body += _len_field(3, pi)
    resp, st, msg = rc_call(6, body, timeout_ms=8000)
    if st != 1:
        return {"ok": False, "error": msg}
    out = {"ok": True, "properties": []}
    pr = _first(resp, 7)
    if pr is not None:
        for wire, item in _decode(pr).get(1, []):
            pf = _decode(item)
            ident_raw = _first(pf, 1)
            entry = {}
            if ident_raw is not None:
                idf = _decode(ident_raw)
                ps, p = _first(idf, 1), _first(idf, 2)
                if isinstance(ps, bytes):
                    entry["property_set"] = ps.decode("utf-8", "replace")
                if isinstance(p, bytes):
                    entry["property"] = p.decode("utf-8", "replace")
            data = _first(pf, 2)
            if isinstance(data, bytes):
                entry["data_hex"] = data.hex()
                entry["data_len"] = len(data)
            else:
                entry["data_hex"] = None  # property absent; container is still SUCCESSFUL
            out["properties"].append(entry)
    return out


def tool_screenshot(args):
    pid = game_pid()
    if not pid:
        return {"ok": False, "error": "game not running"}
    hwnd = game_hwnd(pid)
    if not hwnd:
        return {"ok": False, "error": "no visible game window"}
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = os.path.join(SHOT_DIR, time.strftime("g3_%Y%m%d_%H%M%S.png"))
    path, size = capture_window(hwnd, path)
    return {"ok": True, "path": path, "size": list(size)}


def tool_logs(args):
    which = args.get("which", "paru")
    n = int(args.get("lines", 60))
    candidates = {
        "paru": os.path.join(DOC_DIR, "paru", "g3debug.log"),
        "paru_patch": os.path.join(DOC_DIR, "paru", "paru_s2.log"),
        "crash": os.path.join(DOC_DIR, "Lastlog_GE3.log"),
    }
    path = candidates.get(which)
    if not path or not os.path.exists(path):
        avail = {k: os.path.exists(v) for k, v in candidates.items()}
        return {"ok": False, "error": f"log '{which}' not found", "available": avail,
                "note": "engine logging is off by default; enable Logging.Gothic in ini/paru.ini + Debug.Filter in ge3.ini"}
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    return {"ok": True, "path": path, "tail": lines[-n:]}


def tool_quit(args):
    pid = game_pid()
    if pid:
        @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
        def cb(hwnd, lp):
            p = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
            return True
        user32.EnumWindows(cb, 0)
        for _ in range(20):
            time.sleep(1)
            if not game_pid():
                break
    still = game_pid()
    if still and args.get("force"):
        subprocess.run(["taskkill", "/f", "/pid", str(still)], capture_output=True)
        time.sleep(1)
        still = game_pid()
    restored = False
    if os.path.exists(UO_BACKUP) and os.path.exists(USER_OPTIONS):
        open(USER_OPTIONS, "w", encoding="ascii", errors="replace").write(
            open(UO_BACKUP, encoding="ascii", errors="replace").read())
        os.remove(UO_BACKUP)
        restored = True
    return {"ok": not still, "still_running": bool(still), "useroptions_restored": restored}


TOOLS = {
    "g3_launch": (tool_launch, "Launch Gothic 3 (windowed by default) and wait until Script_RemoteControl is up. Args: width, height, window_mode (0 fullscreen/1 windowed/2 borderless), boot_timeout.",
                  {"type": "object", "properties": {"width": {"type": "integer"}, "height": {"type": "integer"}, "window_mode": {"type": "integer"}, "boot_timeout": {"type": "number"}}}),
    "g3_status": (tool_status, "Is the game running; is RemoteControl loaded; heartbeat MENU/LOADING/INGAME.", {"type": "object", "properties": {}}),
    "g3_entity": (tool_entity, "Query entity position/name/guid. Args: name | guid (braced form) | focus=true (entity under crosshair). Player is PC_Hero. Needs a loaded world.",
                  {"type": "object", "properties": {"name": {"type": "string"}, "guid": {"type": "string"}, "focus": {"type": "boolean"}}}),
    "g3_move_entity": (tool_move_entity, "Move an entity (not the player) to position [x,y,z], keeping its scale/rotation. Args: name|guid|focus, position, put_to_ground (default true).",
                       {"type": "object", "properties": {"name": {"type": "string"}, "guid": {"type": "string"}, "focus": {"type": "boolean"}, "position": {"type": "array", "items": {"type": "number"}}, "put_to_ground": {"type": "boolean"}}, "required": ["position"]}),
    "g3_goto": (tool_goto, "Teleport the PLAYER to an entity (name/guid) or position [x,y,z]. Args: name | guid | position, put_to_ground (default true).",
                {"type": "object", "properties": {"name": {"type": "string"}, "guid": {"type": "string"}, "position": {"type": "array", "items": {"type": "number"}}, "put_to_ground": {"type": "boolean"}}}),
    "g3_spawn": (tool_spawn, "Spawn an entity from a template. Args: template_name|template_guid, location: position [x,y,z] | at_entity | at_entity_guid (default: player position).",
                 {"type": "object", "properties": {"template_name": {"type": "string"}, "template_guid": {"type": "string"}, "position": {"type": "array", "items": {"type": "number"}}, "at_entity": {"type": "string"}, "at_entity_guid": {"type": "string"}}}),
    "g3_property": (tool_property, "Read raw Genome property data of an entity. Args: name|guid, get=[{property_set, property}] (omit both for the whole entity, set-only for a whole property set). Returns hex blobs (Genome binary serialization).",
                    {"type": "object", "properties": {"name": {"type": "string"}, "guid": {"type": "string"}, "get": {"type": "array", "items": {"type": "object", "properties": {"property_set": {"type": "string"}, "property": {"type": "string"}}}}}}),
    "g3_screenshot": (tool_screenshot, "Capture the game window to a PNG; returns the file path.", {"type": "object", "properties": {}}),
    "g3_logs": (tool_logs, "Tail a game log. Args: which = paru|paru_patch|crash, lines.",
                {"type": "object", "properties": {"which": {"type": "string"}, "lines": {"type": "integer"}}}),
    "g3_quit": (tool_quit, "Close the game gracefully (WM_CLOSE), restore UserOptions. Args: force (taskkill if hung).",
                {"type": "object", "properties": {"force": {"type": "boolean"}}}),
}


# ---------------------------------------------------------------------------
# MCP stdio plumbing
# ---------------------------------------------------------------------------

def mcp_main():
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer

    def send(obj):
        data = json.dumps(obj).encode()
        stdout.write(data + b"\n")
        stdout.flush()

    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        rid = req.get("id")
        method = req.get("method")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": req.get("params", {}).get("protocolVersion", "2024-11-05"),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "g3", "version": "0.1.0"}}})
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            send({"jsonrpc": "2.0", "id": rid, "result": {"tools": [
                {"name": k, "description": d, "inputSchema": s} for k, (f, d, s) in TOOLS.items()]}})
        elif method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments") or {}
            try:
                fn = TOOLS[name][0]
                result = fn(args)
                send({"jsonrpc": "2.0", "id": rid, "result": {
                    "content": [{"type": "text", "text": json.dumps(result, ensure_ascii=False)}]}})
            except Exception as e:  # noqa: BLE001 - surface everything to the client
                send({"jsonrpc": "2.0", "id": rid, "result": {
                    "content": [{"type": "text", "text": json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"})}],
                    "isError": True}})
        elif rid is not None:
            send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": f"unknown method {method}"}})


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] != "mcp":
        # CLI debug mode: python g3_mcp.py <tool> ['{"json":"args"}']
        tool = sys.argv[1]
        args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
        print(json.dumps(TOOLS[tool][0](args), ensure_ascii=False, indent=2))
    else:
        mcp_main()
