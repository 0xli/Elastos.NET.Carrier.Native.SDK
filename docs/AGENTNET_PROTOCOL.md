# AgentNet protocol extensions (Carrier Native SDK)

This documents AgentNet's additions to the Elastos Carrier wire protocol, kept
byte-compatible with the JavaScript SDK (`@decentnetwork/peer`) and the iOS /
Android Beagle apps. Everything here is **forward/backward compatible**: a peer
that predates a field simply reads its default, so mixed-version networks keep
working.

## 1. Client metadata in `userinfo` (proto-version + platform)

### Wire change

`src/carrier/packet.fbs` — `userinfo` gains four APPENDED fields (never reorder
or renumber the existing ones; FlatBuffers compatibility depends on append-only):

```fbs
table userinfo {
    avatar : bool = false;
    name   : string;
    descr  : string;
    phone  : string;
    gender : string;
    email  : string;
    region : string;
    proto_version : uint = 0;   // AgentNet wire-protocol version (0 = legacy)
    platform      : string;     // "ios" | "android" | "js" | "darwin" | "linux" | "win32"
    os_version    : string;     // e.g. "17.5", "14", "node-24.2.0"
    app_version   : string;     // e.g. "beagle-2.3.1", "lan-0.1.167"
}
```

`userinfo` is transported as a `PACKET_TYPE_USERINFO` FlatBuffers body over
toxcore's `PACKET_ID_STATUSMESSAGE` (49), sent on every connect. It is the
recurring profile exchange, so it is the natural carrier for client metadata.

### Code done in this SDK

- `src/carrier/packet.fbs` — the four appended fields (above).
- `src/carrier/packet.c`
  - `struct PacketUserInfo` gains `proto_version`, `platform`, `os_version`,
    `app_version`.
  - The `PACKET_TYPE_USERINFO` **pack** path emits each field only when set, so
    an all-legacy profile stays byte-identical to older builds.
  - The **parse** path reads them (`carrier_userinfo_proto_version(tbl)` etc.);
    absent fields yield the schema default (0 / NULL).
  - Accessors: `packet_get_proto_version` / `_platform` / `_os_version` /
    `_app_version` and the matching `packet_set_*`.
- `src/carrier/packet.h` — declarations for the accessors above.

> After editing `packet.fbs`, regenerate `packet_generated.h` with flatcc (the
> build already does this) so `carrier_userinfo_proto_version_add`,
> `carrier_userinfo_platform`, `carrier_userinfo_platform_is_present`, … exist.

### Remaining app-level wiring (in `carrier.c` + bindings + app)

The packet layer is complete; the SDK still needs to (1) populate our own
metadata when it builds the outgoing `userinfo`, and (2) surface a peer's
metadata to the app. This spans the public API + the iOS/Android bindings, so it
is left for the app-build loop:

1. **Send:** where `carrier.c` builds the self `userinfo` (the
   `notify_friend_...` / `dht_self_set_desc` profile path), call
   `packet_set_proto_version(cp, CARRIER_AGENTNET_PROTO_VERSION)`,
   `packet_set_platform(cp, "ios" /* or "android" */)`,
   `packet_set_os_version(cp, <os>)`, `packet_set_app_version(cp, <app>)`.
   Define `CARRIER_AGENTNET_PROTO_VERSION 1` in `carrier.h` (must match the JS
   `AGENTNET_PROTO_VERSION`).
2. **Receive:** in the `PACKET_TYPE_USERINFO` receive handler
   (`unpack_user_descr` / `notify_friend_status_message_cb`), read the accessors
   and store them on the friend record; expose via a new
   `CarrierFriendInfo` field or a dedicated callback so the app can negotiate
   capabilities and display the peer's platform/build.

The JS side is already live (peer ≥ 0.1.87): it advertises `proto_version=1` +
`platform` and surfaces a friend's metadata in `FriendInfoEvent`.

## 2. Large online files (the 5 MB inline cap)

### The current limit

Online file sends between native peers ride **inline bulk messages**
(`bulkmsg`, a FileModel JSON envelope base64-encoded into the message body).
`carrier.h` caps a bulk message at:

```c
#define CARRIER_MAX_APP_BULKMSG_LEN   (5 * 1024 * 1024)   // 5 MB
```

Base64 inflates ~1.34×, so the real file ceiling is **~3.7 MB**. (Single
non-bulk messages are capped at `CARRIER_MAX_APP_MESSAGE_LEN` = 1024 bytes and
are fragmented into a bulk message above that.)

The JavaScript SDK bypasses this with a **separate toxcore messenger file
transfer** (packet ids `PACKET_ID_FILE_SENDREQUEST=80` / `CONTROL=81` /
`DATA=82`), a chunked, resumable, congestion-controlled stream with no size
cap — this is why JS can send very large files while native is stuck at ~3.7 MB.

### Two ways to give native parity

**Option A — raise the bulk cap (SHIPPED).**
`CARRIER_MAX_APP_BULKMSG_LEN` is now **16 MB** (was 5 MB) on native AND JS
(peer ≥ 0.1.87), so inline sends carry ~11 MB real files. The bulkmsg path
already fragments (1 KB units, shared `tid`, `totalsz` on the first fragment)
and reassembles; the JS receiver now has a proper reorder buffer + retransmit
(≥ 0.1.87, receive window widened to 8192) so multi-fragment bulk messages
survive reordering. Trade-offs: the whole file is buffered in memory (no
streaming / resume), and the reliable window bounds lossy-path throughput. Good
for "moderately large" files with near-zero code.

**Option B — implement messenger file transfer (full parity).**
Port the JS file-transfer wire protocol into the native SDK as a new module
riding `dht_friend_message` (the same reliable channel `send_bulk_message` uses),
using packet ids 80/81/82. This gives streaming, resume, and unlimited size,
byte-compatible with JS `sendFile()`/`acceptFile()`.

The complete, authoritative wire spec is
**`@decentnetwork/peer` `docs/FILE_TRANSFER_PROTOCOL.md`** (reference impl:
`src/compat/filetransfer.ts`). Key points for the native port:
  - `FILE_SENDREQUEST(80)`: `[filenum u8][file_type u32 BE][file_size u64 BE][file_id 32][filename]`.
  - `FILE_CONTROL(81)`: `[send_receive u8][filenum u8][control_type u8][data]`;
    `ACCEPT=0 PAUSE=1 KILL=2 SEEK=3 ACK=4`; ACK data = `[acked_offset u32 BE]`.
  - `FILE_DATA(82)`: `[filenum u8][offset u32 BE][chunk ≤1367]`, receiver
    reassembles by offset.
  - **Native can ignore `FILE_FEC(83)`** and skip the JS side's Vegas/BBR window
    + retransmit: `dht_friend_message` is already reliable + in-order, so the
    native side just streams DATA and the receiver periodically ACKs the
    contiguous high-water offset (required, or the JS sender's window stalls).
    This makes the native module a few hundred lines, not ~800.

**Recommendation:** Option A is shipped for immediate relief. Pursue Option B as
a scoped native project for unlimited/streaming transfers. Gate the choice on the
peer's advertised `proto_version` (§1): reserve `proto_version >= 2` for
"messenger-file-transfer capable" and fall back to inline bulkmsg otherwise.
