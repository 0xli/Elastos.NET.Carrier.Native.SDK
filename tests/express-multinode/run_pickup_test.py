#!/usr/bin/env python
"""One bad offline item must not block the mailbox: end-to-end against two
real Express nodes (loopback, own keys, sqlite) with the native SDK client.

Run with the Express repo's venv python (flask, pynacl, base58):

    ~/StudioProjects/Elastos.CarrierClassic.Express/venv/bin/python \
        tests/express-multinode/run_pickup_test.py [--build-dir build/macos-arm64] [--keep]
        [--core-only] [--binary PATH]

--core-only runs just the first scenario; with --binary pointing at a driver
built from the code before the fix it fails (that is the point).

Scenarios (A = first node, B = second; receiver R; friend F; strangers S1, S2):
  1. core      zero-length M from a stranger ahead of a request on A: the
               request still arrives, A's queue is drained, next pull is quiet
  2. real      on B: M from S1 (not a friend), S1's request, S2's request,
               then three M from F (small, 234 KB, 373 KB). The three arrive
               with the right hashes, both requests arrive, S1's M is held, B
               drained. A later run that accepts S1 delivers the held M
               (across a process restart, i.e. the hold is on disk).
  3. kinds     each bad kind Express can carry, first / middle / last between
               a friend M and a request: every valid item once, queue drained,
               next pull quiet. Kinds: zero-length M, garbage M from a friend,
               garbage M from a stranger, R for another nospam, unknown type.
  4. legacy    the single-line state file from older apps still loads.
"""
import argparse
import base64
import json
import os
import shutil
import sqlite3
import ssl
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_test as rt  # noqa: E402  (Node, Client, check, HOST, ports)

from nacl.public import PrivateKey, PublicKey, Box  # noqa: E402
from base58 import b58encode, b58decode  # noqa: E402


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xffffffff
    return '%08x' % h


def body_for(size):
    return bytes((ord('a') + (k * 7 + k // 251) % 26) for k in range(size))


class Stranger:
    """A keypair that posts straight to Express, like any client could."""
    def __init__(self):
        self.sk = PrivateKey.generate()
        self.userid = b58encode(bytes(self.sk.public_key)).decode()

    def post(self, node, to, payload):
        box = Box(self.sk, PublicKey(b58decode(node.pk)))
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        req = urllib.request.Request(
            'https://{}:{}/{}/{}'.format(rt.HOST, node.port, to, self.userid),
            data=box.encrypt(payload), method='POST',
            headers={'Content-Type': 'application/binary'})
        with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
            assert resp.status == 201, resp.status


def other_nospam_address(address):
    raw = bytearray(b58decode(address))
    raw[32] ^= 0x5a
    raw[36] = 0
    raw[37] = 0
    for i in range(36):
        raw[36 + (i % 2)] ^= raw[i]
    return b58encode(bytes(raw)).decode()


def insert_unknown_type(node, to, frm):
    """Express never stores a type other than M/R; put one in its sqlite directly."""
    con = sqlite3.connect(node.db)
    try:
        ts = int(time.time() * 1000000)
        blob = json.dumps({'type': 'inline', 'data': base64.b64encode(b'\0' * 38 + b'x').decode(),
                           'filename': None, 'content_type': None}).encode()
        con.execute('insert into messages (ts, mid, toId, fromId, typ, msg) values (?,?,?,?,?,?)',
                    (ts, ts, to, frm, 'X', blob))
        con.commit()
    finally:
        con.close()
    time.sleep(0.01)


def parse(out):
    msgs, reqs, held, state = [], [], None, {}
    for line in out.splitlines():
        if line.startswith('MESSAGE '):
            kv = dict(p.split('=', 1) for p in line[8:].split())
            msgs.append(kv)
        elif line.startswith('REQUEST '):
            kv = dict(p.split('=', 1) for p in line[8:].split(' ', 1))
            reqs.append(kv)
        elif line.startswith('HELD '):
            held = int(line[5:])
        elif line.startswith('STATE '):
            parts = line[6:].split()
            if len(parts) == 3:
                state[parts[0]] = (int(parts[1]), int(parts[2]))
    return msgs, reqs, held, state


class PullClient(rt.Client):
    def pull2(self, nodes, wait=8, friends=(), accept=False):
        extra = []
        for f in friends:
            extra += ['--friend', f]
        if accept:
            extra.append('--accept')
        out = self.run('pull', nodes, extra, wait=wait)
        return parse(out)

    def sendmsg(self, nodes, to, text=None, size=0, wait=6):
        extra = ['--to', to]
        if size:
            extra += ['--size', str(size)]
        else:
            extra += ['--hello', text]
        return self.run('sendmsg', nodes, extra, wait=wait)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default=os.path.join(rt.NATIVE_ROOT, 'build', 'macos-arm64'))
    ap.add_argument('--binary', default=None, help='driver to use (default: from --build-dir)')
    ap.add_argument('--express-root', default=os.path.expanduser('~/StudioProjects/Elastos.CarrierClassic.Express'))
    ap.add_argument('--python', default=None)
    ap.add_argument('--loglevel', type=int, default=4)
    ap.add_argument('--core-only', action='store_true')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    args.build_dir = os.path.abspath(args.build_dir)
    args.express_root = os.path.abspath(args.express_root)
    python = args.python or os.path.join(args.express_root, 'venv', 'bin', 'python')
    binary = os.path.abspath(args.binary) if args.binary else os.path.join(
        args.build_dir, 'tests', 'express-multinode', 'express_multinode_test')
    if not os.path.exists(binary):
        sys.exit('binary not found: ' + binary)

    work = os.path.join(args.build_dir, 'express-pickup-run')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    print('work dir: {}\nbinary: {}'.format(work, binary))

    a = rt.Node('nodeA', rt.PORT_A, work, python, args.express_root, 'rsa')
    b = rt.Node('nodeB', rt.PORT_B, work, python, args.express_root, 'rsa')
    check = rt.check
    try:
        print('\n== setup')
        a.start()
        b.start()
        r = PullClient('receiver', work, binary, args.loglevel)
        f = PullClient('friendF', work, binary, args.loglevel)
        s1 = PullClient('strangerS1', work, binary, args.loglevel)
        s2 = PullClient('strangerS2', work, binary, args.loglevel)
        for c in (r, f, s1, s2):
            c.init()
        x = Stranger()
        both = [a, b]
        friends = [f.userid]

        # ---------------------------------------------------------- 1. core
        print('\n== 1. a zero-length message from a stranger ahead of a request (node A)')
        x.post(a, r.userid, b'')                      # the poison: payload of 0 bytes
        s2.send([a], r.address, 'after-the-poison')   # a valid request behind it
        msgs, reqs, held, _ = r.pull2(both)   # plain pull: the old driver has no --friend
        check(any(q.get('from') == s2.userid and q.get('hello') == 'after-the-poison' for q in reqs),
              'the request behind the poison item is delivered')
        check(len(a.rows(r.userid)) == 0, 'node A queue drained (rows for receiver: %d)' % len(a.rows(r.userid)))
        msgs, reqs, held, _ = r.pull2(both, wait=5)
        check(not msgs and not reqs, 'next pull is quiet (msgs=%d reqs=%d)' % (len(msgs), len(reqs)))
        if args.core_only:
            raise SystemExit(0 if not rt.failures else 1)

        # ---------------------------------------------------------- 2. real shape
        print('\n== 2. the real queue shape (node B): stranger M, their R, another R, three friend M')
        s1.sendmsg([b], r.userid, text='early-from-s1')
        s1.send([b], r.address, 'add-me-s1')
        s2.send([b], r.address, 'add-me-s2')
        f.sendmsg([b], r.userid, text='small-from-f')
        f.sendmsg([b], r.userid, size=234 * 1024, wait=10)
        f.sendmsg([b], r.userid, size=373 * 1024, wait=10)
        check(len(b.rows(r.userid)) == 6, 'node B holds 6 items for the receiver (%d)' % len(b.rows(r.userid)))

        msgs, reqs, held, _ = r.pull2(both, friends=friends, wait=12)
        fm = [m for m in msgs if m['from'] == f.userid]
        check(len(fm) == 3 and sorted(int(m['len']) for m in fm) == sorted([len('small-from-f'), 234 * 1024, 373 * 1024]),
              'three friend messages delivered, lengths %s' % sorted(int(m['len']) for m in fm))
        check(all(m['fnv'] == fnv1a(body_for(int(m['len']))) for m in fm if int(m['len']) > 100),
              'the two large payloads arrive intact (fnv-1a matches)')
        check(sorted(q['from'] for q in reqs) == sorted([s1.userid, s2.userid]), 'both requests delivered')
        check(not any(m['from'] == s1.userid for m in msgs), "the stranger's message is not delivered yet")
        check(held == 1, 'one item held on disk (%s)' % held)
        check(len(b.rows(r.userid)) == 0, 'node B drained')

        print('   -- a later run (process restart) accepts S1')
        msgs, reqs, held, _ = r.pull2(both, friends=friends + [s1.userid], wait=6)
        sm = [m for m in msgs if m['from'] == s1.userid]
        check(len(sm) == 1 and sm[0]['fnv'] == fnv1a(b'early-from-s1'),
              "S1's held message delivered after accept, across a restart")
        check(held == 0, 'hold empty afterwards (%s)' % held)
        check(not reqs and len(msgs) == 1, 'nothing else delivered twice (msgs=%d reqs=%d)' % (len(msgs), len(reqs)))

        # ---------------------------------------------------------- 3. bad kinds
        print('\n== 3. each bad kind first / middle / last between a friend M and a request (node A)')
        kinds = ['zero-length M (stranger)', 'garbage M (friend)', 'garbage M (stranger)',
                 'R other nospam', 'unknown type']
        other_addr = other_nospam_address(r.address)
        fstranger = Stranger()   # a keypair the receiver also has as a friend (for the "garbage from a friend" kind)

        def inject(kind, idx):
            if kind == 'zero-length M (stranger)':
                x.post(a, r.userid, b'')
            elif kind == 'garbage M (friend)':
                fstranger.post(a, r.userid, os.urandom(80))   # from a friend, but not a packet
            elif kind == 'garbage M (stranger)':
                Stranger().post(a, r.userid, os.urandom(80))
            elif kind == 'R other nospam':
                Stranger().post(a, other_addr, b'junk-hello')
            elif kind == 'unknown type':
                insert_unknown_type(a, r.userid, x.userid)
            else:
                raise AssertionError(kind)

        friends3 = friends + [fstranger.userid]
        rnd = 0
        for kind in kinds:
            for pos in (0, 1, 2):
                rnd += 1
                if pos == 0:
                    inject(kind, rnd)
                f.sendmsg([a], r.userid, text='m-%d' % rnd)
                if pos == 1:
                    inject(kind, rnd)
                s2.send([a], r.address, 'r-%d' % rnd)
                if pos == 2:
                    inject(kind, rnd)
                before = len(a.rows(r.userid))
                msgs, reqs, held, _ = r.pull2(both, friends=friends3, wait=6)
                ok_m = [m for m in msgs if m['from'] == f.userid and m['fnv'] == fnv1a(('m-%d' % rnd).encode())]
                ok_r = [q for q in reqs if q['from'] == s2.userid and q['hello'] == 'r-%d' % rnd]
                after = len(a.rows(r.userid))
                check(len(ok_m) == 1 and len(ok_r) == 1 and len(msgs) == 1 and len(reqs) == 1 and after == 0,
                      '%-26s %-6s: %d rows -> msgs=%d reqs=%d, rows left %d' % (
                          kind, ('first', 'middle', 'last')[pos], before, len(msgs), len(reqs), after))
        msgs, reqs, held, _ = r.pull2(both, friends=friends3, wait=5)
        check(not msgs and not reqs, 'next pull is quiet (msgs=%d reqs=%d)' % (len(msgs), len(reqs)))
        check(len(a.rows(r.userid)) == 0 and len(b.rows(r.userid)) == 0, 'both queues empty')

        # ---------------------------------------------------------- 4. legacy state file
        print('\n== 4. the single-line state file of released apps still loads')
        with open(os.path.join(r.data, 'express_state.dat'), 'w') as fp:
            fp.write('7 0\n')
        msgs, reqs, held, state = r.pull2(both, friends=friends3, wait=5)
        key_a = '{}:{}'.format(rt.HOST, a.port)
        check(state.get(key_a, (None,))[0] == 7, 'node A watermark 7 from the legacy line (%s)' % (state.get(key_a),))

    finally:
        a.stop()
        b.stop()

    print('')
    if rt.failures:
        print('FAIL: {} check(s) failed'.format(len(rt.failures)))
        for x_ in rt.failures:
            print('  - ' + x_)
        print('logs in {}'.format(work))
        sys.exit(1)
    print('PASS: all checks passed')
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
