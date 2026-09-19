# Build dependencies: dead download URLs

The CMake `ExternalProject` steps under `deps/` fetch their sources from
`https://github.com/elastos/...`. Three of those URLs now return **404**, so a
build on a machine without a warm cache fails part-way through:

| Tarball | Size (bytes) | SHA-256 |
|---|---|---|
| `libressl-2.9.2.tar.gz` | 1761077 | `27888e600cf61395d4129692afe2403854a2324d3c26c609d93490fde40ea253` |
| `c-toxcore-0.2.12.carrier.tar.gz` | 501481 | `9a6276d8c4e5202b972d45819fd0b47ff9b55750c8f35a83d54f3ca4a4726381` |
| `curl-7.64.0.tar.bz2` | 3012077 | `d573ba1c2d1cf9d8533fadcce480d778417964e8d04ccddcc76e591d544cf2eb` |

Hashes taken from the copies cached on the Linux group box
(`beagle@10.0.0.115:~/devs/Elastos.NET.Carrier.Native.SDK/build/.tarballs/`),
which predate the URLs breaking. They are the files a working build used, not
upstream-verified: no upstream copy is reachable to compare against.

## The failure is worse than a 404

A failed download leaves a **0-byte file** in `build/.tarballs/`. The next
configure treats that file as the cached download, so the build then fails on a
corrupt archive rather than a missing one, and re-running never recovers. Delete
any zero-length file in `build/.tarballs/` before retrying.

## Working around it today

Copy the three tarballs into `<build-dir>/.tarballs/` before `make`. On a box
with an older SDK checkout, that checkout's `build/.tarballs/` is the source.

Observed on 2026-09-18 building `ce52024` on Ubuntu (gcc 11) for the
CarrierGroup service; every other dependency (cJSON, CUnit, flatcc, libconfig,
libcrystal, libsodium, pjproject, zlib) still downloads fine.

## Fixes worth making

- Mirror the three tarballs somewhere durable and point `deps/*/CMakeLists.txt`
  at it.
- Add `EXPECTED_HASH SHA256=...` to each `ExternalProject_Add`, so a truncated
  or substituted download fails loudly instead of being cached as valid.
