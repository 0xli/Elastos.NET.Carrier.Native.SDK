# express-multinode: two independent Express nodes, one native client

Proves the Express connector (`src/carrier/express.c`) against two
independent offline-message nodes, each with its own keypair, sqlite file and
file store: a message stored on EITHER node reaches the client, each node is
acked (DELETE `?until=`) with its own timestamp only, and a dead node is
skipped rather than blocking the other one. These are the semantics of the
JS client (`peer/packages/peer/src/compat/express.ts`, `pullOnce`).

## Pieces

| file | role |
|---|---|
| `express_multinode_test.c` | native driver: `init` / `send` / `pull` a carrier with N `--node host:port:pk`, no DHT bootstraps, no network beyond the nodes given |
| `express_node.py` | runs the real Express app (`Elastos.CarrierClassic.Express/express.py`) on `127.0.0.1:<port>` over TLS with its own key, sqlite file and storage dir |
| `run_test.py` | orchestrator: starts two nodes, drives three carriers, asserts on the sqlite rows and on the client's `express_state.dat` |

The driver uses offline *friend requests* (`express_enqueue_post_request`,
the path `carrier_add_friend` takes when the friend is offline), because a
friend *message* is only delivered to an existing friend and making friends
needs the DHT. The pull side is the production path: `carrier_run` pulls on
its first iteration and the test enqueues one more pull explicitly.

## Build

The driver calls connector internals that the shared library hides
(`-fvisibility=hidden`), so it links the static `libcarrier.a`. Configure
with tests on, then build just this target:

```bash
cmake -S . -B build/macos-arm64 -DENABLE_TESTS=ON
```

```bash
make -C build/macos-arm64 express_multinode_test
```

## Run

Needs the Express repo's venv (flask, pynacl, base58; `flatbuffers` is
vendored in the repo) and `openssl` on PATH for the self-signed certs.

```bash
~/StudioProjects/Elastos.CarrierClassic.Express/venv/bin/python tests/express-multinode/run_test.py
```

Options: `--build-dir`, `--express-root`, `--python`, `--cert-type rsa|ecdsa`,
`--loglevel N` (carrier log level, default 4), `--keep` (keep
`<build-dir>/express-multinode-run/` with the node and SDK logs).

## What is asserted

A = first configured node (`:15005`), B = second (`:15006`).

1. **post, both up** — the row lands on A only. Posting is first-success in
   the *configured* order, not the latency-sorted order, so phones on old
   builds (which read one node) keep finding new messages on the first node.
2. **post, A down** — the row lands on B.
3. **pull, A down** — B's request is delivered, B's queue is emptied by a
   DELETE sent to B, A's row is untouched, and the state file shows B's
   watermark with A's still 0.
4. **pull, A back** — A's request, whose timestamp is *older* than B's
   watermark, is delivered. A single shared watermark (the pre-change code)
   would have dropped it as a replay. A is then emptied by a DELETE sent to
   A; B's watermark is unchanged.
5. **legacy state file** — a single-line `<last> <pending>` file (the format
   written by commit 290779b) loads onto the first configured node and is
   rewritten in the per-node `host:port last pending` format.

## TLS note

`--cert-type ecdsa` is the check for the LibreSSL/curl bump: with the
bundled LibreSSL 2.9.2 + curl 7.64.0 every request to an ECDSA-certificate
host fails with `CURLE: 35` (SSL connect error), which is why
`lens.beagle.chat` pins an RSA certificate. The SDK does not verify the
server certificate (`CURLOPT_SSL_VERIFYPEER 0`), so a self-signed cert is
enough for the test.
