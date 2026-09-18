// Verify a signature produced by the C SDK's carrier_identity_sign() with the
// JavaScript peer's primitive (curve25519-js). Run from a package tree that
// has curve25519-js, e.g. beagle-web:
//   node --input-type=module -e "$(cat tools/xeddsa-verify.mjs)" < sig.json
import { verify } from "curve25519-js";
import { readFileSync } from "node:fs";
const j = JSON.parse(readFileSync(0, "utf8"));
const h = (s) => Uint8Array.from(Buffer.from(s, "hex"));
const ok = verify(h(j.pk), new TextEncoder().encode(j.msg), h(j.sig));
console.log(ok ? "JS verify: OK" : "JS verify: FAILED");
process.exit(ok ? 0 : 1);
