import { sign, verify } from "curve25519-js";
import nacl from "tweetnacl";
const hex = (b) => Buffer.from(b).toString("hex");
const sk = Uint8Array.from({ length: 32 }, (_, i) => i + 1);           // 01..20
const rnd = Uint8Array.from({ length: 64 }, (_, i) => (i * 7) & 0xff);
const pk = nacl.box.keyPair.fromSecretKey(sk).publicKey;               // X25519 public key = userid bytes
const msgs = [
  "decent-auth\nhttps://app.beagle.chat\nnonce-1234",
  "x",
  "{\"name\":\"air.beagles.eth\",\"owner\":\"userid\"}",
];
const out = { sk: hex(sk), pk: hex(pk), rnd: hex(rnd), cases: [] };
for (const m of msgs) {
  const mb = new TextEncoder().encode(m);
  const sig = sign(sk, mb, rnd);
  if (!verify(pk, mb, sig)) throw new Error("self-verify failed");
  out.cases.push({ msg: m, sig: hex(sig) });
}
// a second key with the Ed25519 sign bit set, so that path is covered too
let sk2, pk2, tries = 0;
do { sk2 = nacl.randomBytes(32); pk2 = nacl.box.keyPair.fromSecretKey(sk2).publicKey; tries++;
  var sig2 = sign(sk2, new TextEncoder().encode("bit"), rnd);
} while ((sig2[63] & 0x80) === 0 && tries < 64);
out.signbit = { sk: hex(sk2), pk: hex(pk2), msg: "bit", sig: hex(sig2), set: (sig2[63] & 0x80) !== 0 };
console.log(JSON.stringify(out, null, 1));
