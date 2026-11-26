#!/usr/bin/env python
"""carrier_express_pull_now(): a message waiting on an Express node is
delivered at once instead of at the next pull timer (2-5 minutes), and a
burst of calls costs at most the pull in progress plus one more.

End to end against two real Express nodes (loopback, own keys, sqlite) with
the native SDK driver in `pullnow` mode. Run with the Express repo's venv:

    ~/StudioProjects/Elastos.CarrierClassic.Express/venv/bin/python \
        tests/express-multinode/run_pullnow_test.py [--build-dir build/macos-arm64] [--keep]

The messages are posted by the friend's driver BEFORE the receiver starts and
taken out of the nodes' sqlite files; each scenario puts one back with a fresh
timestamp. That way no second SDK process runs next to the receiver: the SDK
also pulls whenever its DHT connects, and two drivers on one machine find each
other by LAN discovery. If some other Carrier node on the machine still causes
such a pull inside the quiet window, the scenario is repeated.

Scenarios (A = first node, B = second; receiver R runs the whole time):
  1. a message stored on B is NOT delivered by itself in the next seconds
     (the timer is minutes away); one pull_now delivers it in under 2 s and
     B's queue is emptied;
  2. the same for a message stored on A (pull_now visits every node);
  3. 20 calls in a row: all return 0, and each node sees at most 2 GETs;
  4. two bursts while a message is being stored: nothing is lost;
  5. without any Express node, pull_now returns -1 / ERROR_NOT_EXIST.
"""
import argparse
import os
import shutil
import sqlite3
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_test as rt  # noqa: E402
import run_pickup_test as rp  # noqa: E402  (PullClient, fnv1a)


class Receiver:
    """The driver in pullnow mode: stays up, takes commands, lines are timed."""
    def __init__(self, client, nodes, friends, loglevel):
        cmd = [client.binary, 'pullnow', '--data', client.data, '--loglevel', str(loglevel)]
        for n in nodes:
            cmd += ['--node', n.spec]
        for f in friends:
            cmd += ['--friend', f]
        self.log = open(client.logpath, 'a')
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, text=True, bufsize=1)
        self.lines = []          # (time, line)
        self.cond = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip('\n')
            self.log.write(line + '\n')
            with self.cond:
                self.lines.append((time.time(), line))
                self.cond.notify_all()

    def command(self, text):
        self.proc.stdin.write(text + '\n')
        self.proc.stdin.flush()

    def wait_line(self, prefix, start=0, timeout=10):
        """First line with `prefix` at index >= start: (index, time, kv) or None."""
        deadline = time.time() + timeout
        with self.cond:
            while True:
                for i in range(start, len(self.lines)):
                    t, line = self.lines[i]
                    if line.startswith(prefix):
                        kv = dict(p.split('=', 1) for p in line[len(prefix):].split() if '=' in p)
                        return i, t, kv
                left = deadline - time.time()
                if left <= 0:
                    return None
                self.cond.wait(left)

    def count(self, prefix, start=0):
        with self.cond:
            return sum(1 for _, line in self.lines[start:] if line.startswith(prefix))

    def mark(self):
        with self.cond:
            return len(self.lines)

    def stop(self):
        try:
            self.command('quit')
            self.proc.wait(15)
        except Exception:
            self.proc.kill()
        self.log.close()


def take_rows(node, to):
    """Remove and return the rows queued for `to` on this node."""
    con = sqlite3.connect(node.db)
    try:
        rows = con.execute('select mid, toId, fromId, typ, msg from messages where toId = ? order by ts',
                           (to,)).fetchall()
        con.execute('delete from messages where toId = ?', (to,))
        con.commit()
        return rows
    finally:
        con.close()


def put_row(node, row):
    """Store a row again, as if it had just been posted."""
    con = sqlite3.connect(node.db)
    try:
        ts = int(time.time() * 1000000)
        con.execute('insert into messages (ts, mid, toId, fromId, typ, msg) values (?,?,?,?,?,?)',
                    (ts,) + tuple(row))
        con.commit()
    finally:
        con.close()


def gets(node, userid):
    """GET /<userid> requests the node has answered so far (its own access log)."""
    node.log.flush()
    needle = '"GET /{} '.format(userid)
    with open(node.log.name, errors='replace') as f:
        return sum(1 for line in f if needle in line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default=os.path.join(rt.NATIVE_ROOT, 'build', 'macos-arm64'))
    ap.add_argument('--binary', default=None)
    ap.add_argument('--express-root', default=os.path.expanduser('~/StudioProjects/Elastos.CarrierClassic.Express'))
    ap.add_argument('--python', default=None)
    ap.add_argument('--loglevel', type=int, default=5)
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    args.build_dir = os.path.abspath(args.build_dir)
    args.express_root = os.path.abspath(args.express_root)
    python = args.python or os.path.join(args.express_root, 'venv', 'bin', 'python')
    binary = os.path.abspath(args.binary) if args.binary else os.path.join(
        args.build_dir, 'tests', 'express-multinode', 'express_multinode_test')
    if not os.path.exists(binary):
        sys.exit('binary not found: ' + binary)

    work = os.path.join(args.build_dir, 'express-pullnow-run')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    print('work dir: {}\nbinary: {}'.format(work, binary))

    a = rt.Node('nodeA', rt.PORT_A, work, python, args.express_root, 'rsa')
    b = rt.Node('nodeB', rt.PORT_B, work, python, args.express_root, 'rsa')
    check = rt.check
    recv = None
    try:
        print('\n== setup')
        a.start()
        b.start()
        r = rp.PullClient('receiver', work, binary, args.loglevel)
        f = rp.PullClient('friendF', work, binary, args.loglevel)
        r.init()
        f.init()
        # The friend posts now, while the receiver is not running; the rows
        # are set aside and put back one at a time below.
        f.sendmsg([b], r.userid, text='stored-on-B', wait=3)
        f.sendmsg([a], r.userid, text='stored-on-A', wait=3)
        f.sendmsg([b], r.userid, text='during-bursts', wait=3)
        on_b = take_rows(b, r.userid)
        on_a = take_rows(a, r.userid)
        check(len(on_b) == 2 and len(on_a) == 1, 'three messages posted by the friend and set aside')

        recv = Receiver(r, [a, b], [f.userid], args.loglevel)
        check(recv.wait_line('READY', timeout=20) is not None, 'receiver is running')
        time.sleep(3)            # the pull carrier_run does on its first iteration
        print('  after start-up: {} GET on A, {} on B'.format(gets(a, r.userid), gets(b, r.userid)))

        def deliver(node, name, row, text):
            for attempt in range(1, 4):
                start = recv.mark()
                put_row(node, row)
                time.sleep(4)
                if recv.count('MESSAGE ', start):
                    print('  (attempt {}: the SDK pulled by itself, a DHT connect on this machine; again)'.format(attempt))
                    time.sleep(2)
                    continue
                check(len(node.rows(r.userid)) == 1,
                      'stored on {} and not delivered by itself in 4 s (the pull timer is minutes away)'.format(name))
                recv.command('pull')
                ack = recv.wait_line('PULLNOW ', start, timeout=5)
                check(ack is not None and ack[2].get('ok') == '1', 'pull_now returns 0')
                got = recv.wait_line('MESSAGE ', start, timeout=10)
                if got and ack:
                    ms = int(got[2]['t']) - int(ack[2]['t'])
                    print('  delivered {} ms after pull_now returned'.format(ms))
                    check(got[2].get('fnv') == rp.fnv1a(text.encode()) and got[2].get('offline') == '1',
                          'the message arrives, as an offline message')
                    check(ms < 2000, 'in under 2 s')
                else:
                    check(False, 'the message arrives after pull_now')
                time.sleep(1)
                check(len(node.rows(r.userid)) == 0, "{}'s queue is emptied".format(name))
                check(recv.count('MESSAGE ', start) == 1, 'delivered exactly once')
                return
            check(False, 'no quiet window in 3 attempts on {}'.format(name))

        print('\n== 1. message on the second node, then pull_now')
        deliver(b, 'B', on_b[0], 'stored-on-B')

        print('\n== 2. message on the first node, then pull_now')
        deliver(a, 'A', on_a[0], 'stored-on-A')

        print('\n== 3. 20 calls in a row')
        time.sleep(1)
        start = recv.mark()
        ga, gb = gets(a, r.userid), gets(b, r.userid)
        recv.command('pull 20')
        ack = recv.wait_line('PULLNOW ', start, timeout=5)
        check(ack is not None and ack[2].get('calls') == '20' and ack[2].get('ok') == '20',
              'all 20 calls return 0')
        time.sleep(4)
        da, db = gets(a, r.userid) - ga, gets(b, r.userid) - gb
        print('  GETs caused: {} on A, {} on B'.format(da, db))
        check(1 <= da <= 2 and 1 <= db <= 2, 'between 1 and 2 pulls of each node, not 20')
        check(recv.count('MESSAGE ', start) == 0, 'and nothing delivered twice')

        print('\n== 4. bursts around a message being stored')
        start = recv.mark()
        ga, gb = gets(a, r.userid), gets(b, r.userid)
        recv.command('pull 10')
        put_row(b, on_b[1])
        recv.command('pull 10')
        got = recv.wait_line('MESSAGE ', start, timeout=10)
        check(got is not None and got[2].get('fnv') == rp.fnv1a(b'during-bursts'),
              'the message stored between two bursts is delivered')
        time.sleep(3)
        check(recv.count('MESSAGE ', start) == 1, 'exactly once')
        da, db = gets(a, r.userid) - ga, gets(b, r.userid) - gb
        print('  GETs caused by 20 calls in two bursts: {} on A, {} on B'.format(da, db))
        check(da <= 4 and db <= 4, 'at most 2 pulls per burst')

        recv.stop()
        recv = None

        print('\n== 5. no Express node configured')
        lone = rp.PullClient('lone', work, binary, args.loglevel)
        lone.init()
        lr = Receiver(lone, [], [], args.loglevel)
        check(lr.wait_line('READY', timeout=20) is not None, 'a carrier without Express nodes is running')
        lr.command('pull')
        ack = lr.wait_line('PULLNOW ', timeout=5)
        check(ack is not None and ack[2].get('ok') == '0' and ack[2].get('err') == '0x8100000a',
              'pull_now returns -1 with ERROR_NOT_EXIST')
        lr.stop()
    finally:
        if recv:
            recv.stop()
        a.stop()
        b.stop()
        if not args.keep and not rt.failures:
            shutil.rmtree(work, ignore_errors=True)

    if rt.failures:
        print('\nFAIL: {} check(s) failed'.format(len(rt.failures)))
        for w in rt.failures:
            print('  - ' + w)
        return 1
    print('\nPASS: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
