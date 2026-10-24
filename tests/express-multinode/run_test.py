#!/usr/bin/env python
"""Two independent Express nodes, one native SDK client: does offline mail
stored on EITHER node reach the client, is each node's queue deleted on
that node only, and does a dead node never block the other one?

Run with the Express repo's venv python (needs pynacl, base58, flask):

    ~/StudioProjects/Elastos.CarrierClassic.Express/venv/bin/python \
        tests/express-multinode/run_test.py [--build-dir build/macos-arm64] \
        [--express-root ~/StudioProjects/Elastos.CarrierClassic.Express] \
        [--cert-type rsa|ecdsa] [--keep]

What it checks (A = first configured node, B = second):
  1. post, both up            -> row lands on A only (configured order, not latency)
  2. post, A down             -> row lands on B (first-success failover)
  3. pull, A down             -> B's request delivered; B emptied; A untouched;
                                 the dead node is skipped, not fatal
  4. pull, A back             -> A's OLDER request delivered (per-node watermark:
                                 a single global watermark = B's newer ts would
                                 have dropped it as a replay); A emptied
  5. legacy state file        -> the single-line "<last> <pending>" file from
                                 commit 290779b loads onto node A
These are the semantics of the JS client (peer/packages/peer/src/compat/
express.ts pullOnce): pull every node, ack each node with its own timestamp.
"""
import argparse
import os
import shutil
import sqlite3
import ssl
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

PORT_A = 15005
PORT_B = 15006
HOST = '127.0.0.1'

failures = []


def check(cond, what):
    tag = 'ok  ' if cond else 'FAIL'
    print('  [{}] {}'.format(tag, what), flush=True)
    if not cond:
        failures.append(what)


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def keygen():
    from nacl.public import PrivateKey
    from base58 import b58encode
    sk = PrivateKey.generate()
    return b58encode(bytes(sk)).decode(), b58encode(bytes(sk.public_key)).decode()


def make_cert(work, name, cert_type):
    cert = os.path.join(work, name + '-cert.pem')
    key = os.path.join(work, name + '-key.pem')
    if cert_type == 'ecdsa':
        newkey = ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1']
    else:
        newkey = ['-newkey', 'rsa:2048']
    sh(['openssl', 'req', '-x509', '-nodes', '-days', '2', '-subj', '/CN=localhost',
        '-keyout', key, '-out', cert] + newkey)
    return cert, key


class Node:
    def __init__(self, name, port, work, python, express_root, cert_type):
        self.name = name
        self.port = port
        self.sk, self.pk = keygen()
        self.db = os.path.join(work, name + '.db')
        self.storage = os.path.join(work, name + '-files')
        self.cert, self.key = make_cert(work, name, cert_type)
        self.log = open(os.path.join(work, name + '.log'), 'a')
        self.python = python
        self.express_root = express_root
        self.proc = None

    @property
    def spec(self):
        return '{}:{}:{}'.format(HOST, self.port, self.pk)

    def start(self):
        self.proc = subprocess.Popen(
            [self.python, os.path.join(HERE, 'express_node.py'),
             '--express-root', self.express_root, '--port', str(self.port),
             '--sk', self.sk, '--db', self.db, '--storage', self.storage,
             '--cert', self.cert, '--key', self.key],
            stdout=self.log, stderr=subprocess.STDOUT)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                body = urllib.request.urlopen(
                    'https://{}:{}/version'.format(HOST, self.port), context=ctx, timeout=2).read()
                if body == b'Express/0.1':
                    print('  node {} up on :{} pk={}'.format(self.name, self.port, self.pk), flush=True)
                    return
            except Exception:
                if self.proc.poll() is not None:
                    raise RuntimeError('node {} exited, see {}'.format(self.name, self.log.name))
                time.sleep(0.3)
        raise RuntimeError('node {} did not come up, see {}'.format(self.name, self.log.name))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        print('  node {} stopped'.format(self.name), flush=True)

    def rows(self, to=None):
        if not os.path.exists(self.db):
            return []
        con = sqlite3.connect(self.db)
        try:
            q = 'select ts, toId, fromId, typ from messages'
            args = ()
            if to:
                q += ' where toId = ?'
                args = (to,)
            return con.execute(q + ' order by ts', args).fetchall()
        finally:
            con.close()

    def wait_rows(self, to, count, timeout=20):
        deadline = time.time() + timeout
        rows = self.rows(to)
        while len(rows) < count and time.time() < deadline:
            time.sleep(0.3)
            rows = self.rows(to)
        return rows


class Client:
    def __init__(self, name, work, binary, loglevel):
        self.name = name
        self.data = os.path.join(work, name + '-data')
        os.makedirs(self.data, exist_ok=True)
        self.binary = binary
        self.loglevel = loglevel
        self.logpath = os.path.join(work, name + '-sdk.log')
        self.address = None
        self.userid = None

    def run(self, mode, nodes, extra=(), wait=None):
        cmd = [self.binary, mode, '--data', self.data, '--loglevel', str(self.loglevel)]
        for n in nodes:
            cmd += ['--node', n.spec]
        if wait is not None:
            cmd += ['--wait', str(wait)]
        cmd += list(extra)
        res = subprocess.run(cmd, capture_output=True, text=True)
        with open(self.logpath, 'a') as f:
            f.write('$ {}\n{}\n{}\n'.format(' '.join(cmd), res.stdout, res.stderr))
        if res.returncode != 0:
            raise RuntimeError('{} {} failed rc={}: {}'.format(self.name, mode, res.returncode, res.stderr[-800:]))
        for line in res.stdout.splitlines():
            if line.startswith('address: '):
                self.address = line.split(' ', 1)[1].strip()
            elif line.startswith('userid: '):
                self.userid = line.split(' ', 1)[1].strip()
        return res.stdout

    def init(self):
        self.run('init', [])
        print('  client {}: userid={}'.format(self.name, self.userid), flush=True)

    def send(self, nodes, to, hello, wait=4):
        return self.run('send', nodes, ['--to', to, '--hello', hello], wait=wait)

    def pull(self, nodes, wait=8):
        out = self.run('pull', nodes, wait=wait)
        requests = []
        state = {}
        for line in out.splitlines():
            if line.startswith('REQUEST '):
                kv = dict(p.split('=', 1) for p in line[len('REQUEST '):].split(' ', 1))
                requests.append((kv.get('from'), kv.get('hello')))
            elif line.startswith('STATE '):
                parts = line[len('STATE '):].split()
                if len(parts) == 3:
                    state[parts[0]] = (int(parts[1]), int(parts[2]))
                else:
                    state['_raw'] = line
        return requests, state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default=os.path.join(NATIVE_ROOT, 'build', 'macos-arm64'))
    ap.add_argument('--express-root',
                    default=os.path.expanduser('~/StudioProjects/Elastos.CarrierClassic.Express'))
    ap.add_argument('--python', default=None, help='python with flask+pynacl (default: express venv)')
    ap.add_argument('--cert-type', choices=['rsa', 'ecdsa'], default='rsa')
    ap.add_argument('--loglevel', type=int, default=4)
    ap.add_argument('--keep', action='store_true', help='keep the work dir')
    args = ap.parse_args()

    python = args.python or os.path.join(args.express_root, 'venv', 'bin', 'python')
    binary = os.path.join(args.build_dir, 'tests', 'express-multinode', 'express_multinode_test')
    if not os.path.exists(binary):
        sys.exit('binary not found: {} (configure with -DENABLE_TESTS=ON and build express_multinode_test)'.format(binary))

    work = os.path.join(args.build_dir, 'express-multinode-run')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    print('work dir: {}'.format(work))
    print('cert type: {}'.format(args.cert_type))

    a = Node('nodeA', PORT_A, work, python, args.express_root, args.cert_type)
    b = Node('nodeB', PORT_B, work, python, args.express_root, args.cert_type)
    try:
        print('\n== setup: two Express nodes, own keys, own sqlite files')
        a.start()
        b.start()

        receiver = Client('receiver', work, binary, args.loglevel)
        sender1 = Client('sender1', work, binary, args.loglevel)
        sender2 = Client('sender2', work, binary, args.loglevel)
        for c in (receiver, sender1, sender2):
            c.init()
        both = [a, b]

        print('\n== 1. post with both nodes up lands on the FIRST configured node')
        sender1.send(both, receiver.address, 'h1')
        rows_a = a.wait_rows(receiver.userid, 1)
        rows_b = b.rows(receiver.userid)
        check(len(rows_a) == 1 and rows_a[0][2] == sender1.userid, 'node A holds the request from sender1')
        check(len(rows_b) == 0, 'node B holds nothing')
        ts_a = rows_a[0][0] if rows_a else 0

        print('\n== 2. post with node A down fails over to node B')
        a.stop()
        sender2.send(both, receiver.address, 'h2')
        rows_b = b.wait_rows(receiver.userid, 1)
        check(len(rows_b) == 1 and rows_b[0][2] == sender2.userid, 'node B holds the request from sender2')
        ts_b = rows_b[0][0] if rows_b else 0
        check(ts_b > ts_a, 'B\'s row is newer than A\'s (ts_b={} > ts_a={})'.format(ts_b, ts_a))

        print('\n== 3. pull with node A down: B is read and acked, A is skipped')
        reqs, state = receiver.pull(both)
        check((sender2.userid, 'h2') in reqs, 'sender2\'s request delivered from node B')
        check((sender1.userid, 'h1') not in reqs, 'sender1\'s request (on the dead node A) not delivered yet')
        check(len(b.rows(receiver.userid)) == 0, 'node B emptied by DELETE ?until= on B')
        check(len(a.rows(receiver.userid)) == 1, 'node A still holds its row (A was down; nothing sent to it)')
        check(state.get(b.spec.rsplit(':', 1)[0]) == (ts_b, 0),
              'state: B watermark={} pending=0 ({})'.format(ts_b, state.get(b.spec.rsplit(':', 1)[0])))
        check(state.get(a.spec.rsplit(':', 1)[0]) == (0, 0),
              'state: A watermark untouched at 0 ({})'.format(state.get(a.spec.rsplit(':', 1)[0])))

        print('\n== 4. node A back: its OLDER request is delivered (per-node watermark) and deleted on A only')
        a.start()
        reqs, state = receiver.pull(both)
        check((sender1.userid, 'h1') in reqs,
              'sender1\'s request (ts {} < B watermark {}) delivered from node A'.format(ts_a, ts_b))
        check((sender2.userid, 'h2') not in reqs, 'sender2\'s request not delivered twice')
        check(len(a.rows(receiver.userid)) == 0, 'node A emptied by DELETE ?until= on A')
        check(state.get(a.spec.rsplit(':', 1)[0]) == (ts_a, 0),
              'state: A watermark={} pending=0 ({})'.format(ts_a, state.get(a.spec.rsplit(':', 1)[0])))
        check(state.get(b.spec.rsplit(':', 1)[0]) == (ts_b, 0),
              'state: B watermark still {} ({})'.format(ts_b, state.get(b.spec.rsplit(':', 1)[0])))

        print('\n== 5. legacy single-line state file loads onto the first configured node')
        with open(os.path.join(receiver.data, 'express_state.dat'), 'w') as f:
            f.write('7 0\n')
        reqs, state = receiver.pull(both, wait=5)
        check(reqs == [], 'nothing new delivered')
        check(state.get(a.spec.rsplit(':', 1)[0]) == (7, 0),
              'state: A watermark=7 from the legacy line ({})'.format(state.get(a.spec.rsplit(':', 1)[0])))
        check(state.get(b.spec.rsplit(':', 1)[0]) == (0, 0),
              'state: B starts at 0 (the legacy file knew one node) ({})'.format(state.get(b.spec.rsplit(':', 1)[0])))

    finally:
        a.stop()
        b.stop()

    print('')
    if failures:
        print('FAIL: {} check(s) failed'.format(len(failures)))
        for f in failures:
            print('  - ' + f)
        print('logs in {}'.format(work))
        sys.exit(1)
    print('PASS: all checks passed (cert type {})'.format(args.cert_type))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
