# Gothic 3 Script_RemoteControl — Wire Protocol

Reverse-engineered from:

- Server (in-game DLL): `F:\Projects\Gothic3\gothic3sdk-examples\examples\Script_RemoteControl\`
  - `src\me_ipcadmin.cpp` — ZeroMQ ROUTER loop
  - `src\handler\me_rh_*.cpp` — request handlers
  - `src\proto\g3rc.pb.h/.cc` — **generated protobuf code (protoc 2.6.1) = the truth**
- Java client: `F:\Projects\Gothic3\g3dit\g3dit\src\main\java\de\george\g3dit\rpc\` (GothicIpc.java etc.)
  and `F:\Projects\Gothic3\g3dit\g3dit\src\main\proto\g3rc.proto` (this proto **matches** the generated
  code exactly; the `g3rc.proto` inside gothic3sdk-examples is STALE — it lacks SpawnRequest,
  PropertyRequest, EntityRequest.focus/editor/moveto/put_to_ground, GotoRequest.put_to_ground and the
  EntityResponse name/guid fields).

Native libs used by the server build: **ZeroMQ 4.0.4** (ZMTP 3.0) and **protobuf 2.6.1** (LITE runtime).

---

## 1. Transport: ZeroMQ socket pattern and framing

- Server: `zmq::socket_t(ctx, ZMQ_ROUTER)`, `setsockopt(ZMQ_RCVTIMEO, 0)` (non-blocking),
  **bound to `tcp://127.0.0.1:5555`** (hardcoded, localhost only).
- Client (g3dit reference): **`ZMQ_DEALER`**, `connect("tcp://localhost:5555")`.

### Framing — exactly what bytes go on the socket

The client sends the serialized `RequestContainer` as **ONE single-part ZMQ message. No identity
frame is sent manually, and NO empty delimiter frame** (that is REQ behavior, not DEALER).

- DEALER send: `[serialized RequestContainer]` (1 frame).
- ROUTER receives it as `[auto-generated identity][payload]` (2 frames). The server code reads
  exactly these two frames per request (`recv(Identity)`, `recv(Data)`).
- ROUTER reply: `send(Identity, SNDMORE); send(serialized ResponseContainer)` — the identity frame
  is consumed for routing, so the DEALER client receives **one single frame** containing the
  serialized `ResponseContainer`.

**Do NOT use a REQ socket.** REQ inserts an empty delimiter frame; the server would parse the empty
frame as the request, then treat your actual payload as the *next* request's identity frame and try
to route a reply to a nonexistent peer. Everything desyncs silently. DEALER only, single-part sends.

In Python: use `pyzmq` for the transport (only protobuf is hand-rolled):

```python
import zmq
ctx = zmq.Context()
s = ctx.socket(zmq.DEALER)
s.connect("tcp://127.0.0.1:5555")
s.send(request_bytes)              # single frame
if s.poll(1500):                   # g3dit uses a 1500 ms timeout
    response_bytes = s.recv()      # single frame
```

The g3dit worker polls in a 50 ms loop, sends with `setSendTimeOut(0)`/`setReceiveTimeOut(0)`, and
expires unanswered requests after **1500 ms** (treated as "Gothic 3 is not reachable").
The heartbeat monitor sends one HeartbeatRequest every **250 ms**.

g3dit quirk worth knowing: it detects "accidental self connect" (ephemeral local port == 5555) and
reconnects; with a hand-written client you would just see your own requests echoed back.

---

## 2. Envelope semantics

Every request is a `RequestContainer`; every reply is a `ResponseContainer`.

- `request_number` (uint32, required): **client-chosen correlation id, echoed verbatim** in the
  response. The server does nothing else with it. g3dit uses an `AtomicInteger` starting at 1
  (`incrementAndGet`). Recommended: start at 1 and increase monotonically; avoid 0 (that is what a
  failed/empty parse on the server side would echo, so 0 is ambiguous).
- Exactly one of the `request` oneof fields (2..6) must be set. The server dispatches on
  `request_case()`.
- Replies come back in processing order; because all pending requests are drained once per game
  frame, several replies can arrive back-to-back in a burst. Match on `request_number`.
- If `request_case` is unknown/not set (garbage bytes, empty payload, a field number the server
  does not know), the server **still replies**, but with a `ResponseContainer` that has only
  `request_number` set — the required `status` field is missing, i.e. the reply is not a valid
  proto2 message. A strict parser throws (g3dit logs and skips it, then the request times out).
  A hand-rolled parser should tolerate a missing `status`.
- If a handler runs but sets no response payload, the server auto-fills an empty `void_response`.

---

## 3. Messages (from the GENERATED pb code — proto2, package `g3rc`)

Wire types used: `V` = varint (0), `F32` = 32-bit fixed (5), `LEN` = length-delimited (2).
All submessages, strings, bytes are `LEN`. Floats are IEEE-754 single, little-endian. Bools are
varint 0/1. Enums are varints. proto2: required fields are always written; optional only when set.

### RequestContainer
| field | # | type | wire |
|---|---|---|---|
| request_number | 1 | required uint32 | V |
| heartbeat_request | 2 | HearbeatRequest (oneof request) | LEN |
| entity_request | 3 | EntityRequest (oneof request) | LEN |
| goto_request | 4 | GotoRequest (oneof request) | LEN |
| spawn_request | 5 | SpawnRequest (oneof request) | LEN |
| property_request | 6 | PropertyRequest (oneof request) | LEN |

### ResponseContainer
| field | # | type | wire |
|---|---|---|---|
| request_number | 1 | required uint32 | V |
| status | 2 | required enum Status | V |
| message | 3 | optional string (UTF-8; error text, in German) | LEN |
| heartbeat_response | 4 | HearbeatResponse (oneof response) | LEN |
| entity_response | 5 | EntityResponse (oneof response) | LEN |
| void_response | 6 | VoidResponse (oneof response) | LEN |
| property_response | 7 | PropertyResponse (oneof response) | LEN |

enum `ResponseContainer.Status`: `FAILED = 0`, `SUCCESSFUL = 1`.

### HearbeatRequest *(sic — typo is in the proto)*
Empty message (serializes to 0 bytes).

### EntityRequest  → EntityResponse
| field | # | type | wire |
|---|---|---|---|
| name | 1 | string (oneof identifier) | LEN |
| guid | 2 | string (oneof identifier) | LEN |
| focus | 3 | bool (oneof identifier) | V |
| editor | 6 | bool (oneof identifier) | V |
| moveto | 4 | optional Position | LEN |
| put_to_ground | 5 | optional bool | V |

### GotoRequest  → VoidResponse
| field | # | type | wire |
|---|---|---|---|
| name | 1 | string (oneof identifier) | LEN |
| guid | 2 | string (oneof identifier) | LEN |
| position | 3 | Vector (oneof identifier) | LEN |
| put_to_ground | 4 | optional bool | V |

### SpawnRequest  → EntityResponse
| field | # | type | wire |
|---|---|---|---|
| template_name | 1 | string (oneof identifier) | LEN |
| template_guid | 2 | string (oneof identifier) | LEN |
| entity_name | 3 | string (oneof location) | LEN |
| entity_guid | 4 | string (oneof location) | LEN |
| position | 5 | Vector (oneof location) | LEN |

### PropertyRequest  → PropertyResponse
| field | # | type | wire |
|---|---|---|---|
| entity_name | 1 | string (oneof identifier) | LEN |
| entity_guid | 2 | string (oneof identifier) | LEN |
| properties_get | 3 | repeated PropertyIdentifier | LEN |
| properties_set | 4 | repeated PropertySerialized | LEN |

### HearbeatResponse
| field | # | type | wire |
|---|---|---|---|
| status | 1 | required enum Status | V |

enum `HearbeatResponse.Status`: `MENU = 0`, `LOADING = 1`, `INGAME = 2`.
**The current handler never sends LOADING** — it answers MENU when `GUIManager->IsMenuOpen()`,
otherwise INGAME. During an actual loading screen you get no answer at all (see lifecycle).

### VoidResponse
Empty message.

### EntityResponse
| field | # | type | wire |
|---|---|---|---|
| position | 1 | required Position | LEN |
| name | 2 | required string | LEN |
| guid | 3 | required string | LEN |

### PropertyResponse
| field | # | type | wire |
|---|---|---|---|
| properties_get | 1 | repeated PropertySerialized | LEN |
| properties_set | 2 | repeated PropertySerialized | LEN |

### Position
| field | # | type | wire |
|---|---|---|---|
| translation | 1 | required Vector | LEN |
| scale | 2 | required Vector | LEN |
| rotation | 3 | required EulerAngles | LEN |

### Vector
`x = 1`, `y = 2`, `z = 3` — all required float, wire type 5 (LE IEEE-754 single).

### EulerAngles
`yaw = 1`, `pitch = 2`, `roll = 3` — all required float, wire type 5.

### PropertyIdentifier
| field | # | type | wire |
|---|---|---|---|
| property_set | 1 | optional string | LEN |
| property | 2 | optional string | LEN |

Semantics: neither set → whole **entity**; only `property_set` → whole **property set**
(e.g. `"gCInventory_PS"`); both → a single **property** of that set.

### PropertySerialized
| field | # | type | wire |
|---|---|---|---|
| identifier | 1 | required PropertyIdentifier | LEN |
| data | 2 | optional bytes | LEN |

`data` is Genome engine binary serialization (NOT protobuf), little-endian:
- single property: `bCString` propertyName + `bCString` propertyClassName + raw `PropertyWrite`
  payload, where a `bCString` on the stream = `uint16 length` + that many raw ANSI bytes (no NUL);
- property set: `bCAccessorPropertyObject` serialization (versioned; g3dit reads it with
  `ClassUtil.readSubClass`);
- whole entity: `bCString` entity type — `"Dynamic"`, `"Spatial"` or `"Template"` — followed by
  `eCEntity::Write` output.
On a get, a missing/unknown property yields a `PropertySerialized` whose `data` field is absent —
that is the "not found" signal (the container status is still SUCCESSFUL).

---

## 4. GUID string format

All `guid` string fields carry the **Genome GUID text form with braces**:
`{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}` (g3dit produces it lowercase; the first three groups are
byte-swapped relative to the raw 20-byte hex storage, i.e. standard Windows GUID text). The server
parses it via `bCGuid(FromUTF8(guid))`. g3dit always converts through
`GuidUtil.hexToGuidText(GuidUtil.parseGuid(x))` before putting a guid on the wire.
Response `EntityResponse.guid` is `bCPropertyID::GetText()` — same braced text form.

---

## 5. Handler behavior

Dispatch: `me_ipcadmin.cpp` maps `request_case` → handler. All handlers run on the **engine main
thread** during the per-frame module `Process()`.

Entity lookup used everywhere (`me_requesthandler.cpp`): `eCSceneAdmin::GetEntityByName(name)`
— exact **world entity name** — with fallback `GetEntityByPartName(name, hint)` (substring/partial
name match). Guid lookup: `eCSceneAdmin::GetEntity(bCPropertyID(bCGuid(text)))`. These are world
entities of the loaded level, not focus-name strings; the player entity is named **`PC_Hero`**
(that is what g3dit queries for its live position overlay).

- **Heartbeat** (`me_rh_heartbeat.cpp`): replies `heartbeat_response.status` = MENU if the GUI menu
  is open, else INGAME. Never fails, never sends LOADING.
- **Entity** (`me_rh_entity.cpp`): resolves the entity by `name` / `guid` / `focus` (the entity the
  player currently focuses, via player's `gCFocus_PS`) / `editor` (current entity of the in-game
  session editor). If `moveto` is present, **teleports/re-poses that entity** to the given
  Position (rotation+scale+translation composed into a world matrix). If `put_to_ground=true`,
  temporarily adds RigidBody/CollisionShape property sets and dynamic physics so the entity drops
  to the ground; physics is restored after it settles (hard 5 s cap, polled per frame).
  Success → `entity_response` with current world position/scale/rotation, name and guid.
  Failure → status=FAILED, message `"Entity konnte nicht gefunden werden."` (no in-game print).
- **Goto** (`me_rh_goto.cpp`): resolves a target position from `name`/`guid` (that entity's world
  position) or a literal `position` vector, then **teleports the PLAYER** (`gCSession::GetPlayer()`)
  there. The named entity itself never moves. With `put_to_ground=true` the handler raycasts down
  from y=1,000,000 over up to 200 frames (jiggling the player through y layers to force physics
  streaming) and snaps the player to the first hit. Success → `void_response` (sent immediately,
  before the ground snap completes). Failure → FAILED + in-game message `"Ziel-Entity nicht
  gefunden."`.
- **Spawn** (`me_rh_spawn.cpp`) — present in generated code, missing from the stale proto:
  resolves a spawn position from `entity_name`/`entity_guid` (that entity's world position) or
  literal `position`; resolves a **template** by `template_name`
  (`eCSceneAdmin::GetTemplateEntityByName`) or `template_guid` (entity lookup with
  TemplateEntity hint); creates a dynamic entity from the template (using the template's single
  child as the real template if it has exactly one child) and places it at the position.
  Success → `entity_response` for the newly spawned entity (its fresh guid!). Failures → FAILED +
  in-game message (`"Template konnte nicht gefunden werden."` / `"Spawn-Position konnte nicht
  ermittelt werden."`).
- **Property** (`me_rh_property.cpp`): resolves entity by `entity_name`/`entity_guid`; for each
  `properties_get` entry appends a `PropertySerialized` (see §3) to `response.properties_get`;
  for each `properties_set` entry deserializes and applies the Genome blob (name+class must match
  for single properties), then reads it back into `response.properties_set` (so the reply shows
  the post-set state). Entity not found → FAILED + in-game message.

---

## 6. Lifecycle

- **Bind timing**: `Script_RemoteControl.dll` is a Gothic 3 script DLL. Its exported
  `ScriptInit()` runs when the engine loads script libraries during game startup (before the main
  menu). It registers `mCIpcAdmin` via a `bCAccessorCreator`, which constructs the instance
  immediately; the constructor itself calls `PostInitialize()` → **the ROUTER socket is bound to
  127.0.0.1:5555 at script-load time**, and the component registers with `eCModuleAdmin`.
- **Processing**: `mCIpcAdmin::Process()` is the standard `eCEngineComponentBase` per-frame hook,
  pumped by the engine module admin **once per game frame on the main thread**. There is no
  dedicated network thread. Each frame it: (1) runs handler `Process()` follow-ups (put_to_ground
  retries), (2) drains **all** queued requests non-blockingly (`ZMQ_RCVTIMEO=0`) and answers each
  one synchronously.
- **Latency** = up to one frame in-game. Multiple outstanding requests are fine; they queue in the
  socket and are all answered within one frame.
- **MENU state**: the module IS processed in the main menu — heartbeat answers `MENU`, and other
  requests are handled too (entity lookups just fail with FAILED if no world is loaded).
- **LOADING**: while a world is loading/saving the main loop does not pump modules, so requests
  are neither read nor answered — the TCP connection stays up (bind persists), requests simply sit
  in the queue and are answered after loading finishes. Clients must use their own timeout
  (g3dit: 1500 ms → "not reachable"); the `LOADING` heartbeat enum value is dead code.
- **Pause**: same mechanism — if the engine stops pumping modules (e.g. loading screen, some pause
  states), expect timeouts, then a burst of stale replies once processing resumes. Correlate by
  `request_number` and be ready to drop replies you already gave up on (g3dit's
  MonotonicallyOrderedIpc drops replies older than the newest one seen).
- **Shutdown**: `PreShutdown()` closes socket and context.

---

## 7. Hand-rolled proto2 encoding — worked example

Varint: little-endian base-128, MSB = continuation. Tag byte = `(field_number << 3) | wire_type`.

### HeartbeatRequest, request_number = 1

```
RequestContainer {
  request_number = 1        → tag 0x08 (field 1, varint), value 0x01
  heartbeat_request = {}    → tag 0x12 (field 2, LEN), length 0x00
}
wire: 08 01 12 00           (4 bytes, sent as one ZMQ frame)
```

Typical reply while in game (`request_number=1, status=SUCCESSFUL, heartbeat_response{INGAME}`):

```
08 01        request_number = 1        (field 1, varint)
10 01        status = SUCCESSFUL(1)    (field 2, varint)
22 02        heartbeat_response        (field 4, LEN, 2 bytes)
   08 02       status = INGAME(2)      (field 1, varint)
```

### GotoRequest to a literal position (x=1.5, y=2.0, z=-3.25), put_to_ground, request_number = 7

```
08 07                       request_number = 7
22 13                       goto_request (field 4, LEN, 19 bytes)
   1a 0f                      position (field 3, LEN, 15 bytes)
      0d 00 00 c0 3f            x = 1.5   (field 1, fixed32 LE)
      15 00 00 00 40            y = 2.0   (field 2, fixed32 LE)
      1d 00 00 50 c0            z = -3.25 (field 3, fixed32 LE)
   20 01                      put_to_ground = true (field 4, varint)
```

### Minimal Python encoder sketch

```python
import struct

def _varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n: return bytes(out)

def _tag(f, wt): return _varint((f << 3) | wt)
def f_varint(f, v): return _tag(f, 0) + _varint(v)          # uint32/bool/enum
def f_len(f, b):    return _tag(f, 2) + _varint(len(b)) + b # submsg/string/bytes
def f_float(f, v):  return _tag(f, 5) + struct.pack('<f', v)

def heartbeat(reqno):
    return f_varint(1, reqno) + f_len(2, b'')

def entity_by_name(reqno, name):
    return f_varint(1, reqno) + f_len(3, f_len(1, name.encode('utf-8')))

# Decoder: walk tags; on wire type 0 read varint, type 5 read 4 bytes, type 2 read
# varint length then that many bytes (recurse for submessages). Tolerate missing
# required fields (see §2) and unknown field numbers (skip by wire type).
```

---

## 8. Gotchas checklist

1. **DEALER, single frame, no empty delimiter.** REQ breaks the server's 2-frame read loop.
2. The stale `g3rc.proto` next to the C++ sources is missing Spawn/Property and several fields;
   trust `src\proto\g3rc.pb.h/.cc` or g3dit's `g3dit\src\main\proto\g3rc.proto` (identical).
3. Handled one request type per frame batch; replies burst after loading screens — always use
   timeouts + `request_number` correlation, drop late replies.
4. `GotoRequest` teleports the **player** to the named entity, not the entity; use
   `EntityRequest.moveto` to move an arbitrary entity.
5. Goto answers SUCCESSFUL immediately even though `put_to_ground` keeps adjusting the player for
   up to 200 subsequent frames.
6. Spawn with a `template_guid` uses the plain entity-by-guid lookup with a TemplateEntity hint;
   `template_name` uses the dedicated template-name index.
7. Guid strings must be in braced text form `{...}`; names may be partial (substring fallback).
8. Unknown/garbage request → structurally invalid reply (missing required `status`); don't crash
   on it, and don't use `request_number=0`.
9. Error `message` strings are German and UTF-8 encoded.
10. Entity/property requests only make sense INGAME (a loaded world); in MENU they return FAILED.
11. Heartbeat `LOADING` is never sent; loading = silence (timeout).
12. Property `data` blobs are Genome binary (uint16-length-prefixed strings etc.), not protobuf —
    treat them as opaque unless you implement the Genome property serialization.
13. Everything binds to 127.0.0.1 only; remote use needs a local proxy on the game machine.
