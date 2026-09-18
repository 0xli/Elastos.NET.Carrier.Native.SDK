/*
 * XEdDSA over a Carrier identity key (Signal's scheme, as implemented by
 * curve25519-js, which the JavaScript peer and beagle-app/beagle-web use for
 * "Sign in with Beagle" and beagles.eth registration). A Carrier identity is
 * an X25519 key pair; the public half IS the userid. This signs with that
 * private key and verifies against that public key, so a signature is bound
 * to the identity itself — no second key.
 *
 * Byte-for-byte compatible with curve25519-js `sign(sk, msg, random)` /
 * `verify(pk, msg, sig)`: same nonce hash (0xFE ‖ 0xFF*31 ‖ a ‖ msg ‖ Z),
 * same sign-bit-in-s[63] convention.
 */
#ifndef __CARRIER_XEDDSA_H__
#define __CARRIER_XEDDSA_H__

#include <stdint.h>
#include <stddef.h>

#define XEDDSA_SIGNATURE_BYTES 64

/* `random` is 64 bytes, or NULL to draw fresh randomness. Returns 0 / -1. */
int xeddsa_sign(uint8_t sig[XEDDSA_SIGNATURE_BYTES], const uint8_t sk[32],
                const uint8_t *msg, size_t len, const uint8_t *random);

/* Returns 1 if valid, 0 if not. */
int xeddsa_verify(const uint8_t pk[32], const uint8_t *msg, size_t len,
                  const uint8_t sig[XEDDSA_SIGNATURE_BYTES]);

/* Ed25519 public key for an X25519 public key: y = (u - 1) / (u + 1). */
void xeddsa_convert_public_key(uint8_t ed[32], const uint8_t x25519[32]);

#endif
