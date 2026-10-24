# Build dependencies: versions and download URLs

## LibreSSL 4.3.4 + curl 8.23.0 (2026-10-24)

`deps/libressl` and `deps/libcurl` moved from LibreSSL 2.9.2 / curl 7.64.0
(both 2019) to current upstream releases, fetched from the upstream sites:

| Tarball | Size (bytes) | SHA-256 | Source |
|---|---|---|---|
| `libressl-4.3.4.tar.gz` | 9311161 | `57c8b2f8c6f9505587abaeddab003ea243d615edb7ed71957968121fc6316bd6` | `https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/` (hash matches upstream `SHA256`) |
| `curl-8.23.0.tar.bz2` | 3453883 | `95b22eb32bf6d1fa6e3e9463bdb61cece3efc56d86576a9b3cbd428246a6d854` | `https://curl.se/download/` (hash computed from that download) |

Why: LibreSSL 2.9.2 could not complete a TLS handshake with a server that
presents an **ECDSA** certificate (every request failed with `CURLE: 35`), so
the Express hosts (`lens.beagle.chat`, `express.beagle.chat`) had to pin RSA
certificates. With the new pair, `tests/express-multinode/run_test.py
--cert-type ecdsa` passes (P-256 server certs through the SDK's own HTTP
client), and the built `build/macos-arm64/intermediates/bin/curl` completes
TLS 1.3 to `https://www.cloudflare.com/` (ECDSA chain, "signed using
ecdsa-with-SHA256"). Keep the RSA pin on the servers until every phone runs a
build with these deps; after that it is optional.

What changed in the build:

- `deps/libressl`: no patch any more (the old one only bumped
  `cmake_minimum_required`; 4.x asks for CMake 3.16.4 itself). Same CMake
  options (`LIBRESSL_APPS/TESTS=OFF`, static, `ENABLE_ASM=OFF`).
- `deps/libcurl`: curl 8 needs the TLS backend named: `--with-openssl=`
  (covers LibreSSL) replaces `--with-ssl=`; the 7.64-era `--without-
  {winssl,schannel,darwinssl,polarssl,cyassl,libmetalink}` switches are gone;
  added `--without-ca-bundle --without-ca-path` (the SDK does not verify
  peers and cross builds must not bake in a macOS path), `--without-zstd
  --without-ngtcp2 --without-nghttp3 --without-libgsasl`, `--disable-mqtt
  --disable-websockets --disable-docs`.
- curl 8 on **macOS** calls `SCDynamicStoreCopyProxies` at init, so the
  `libcurl` interface target now links `CoreFoundation` and
  `SystemConfiguration` on Apple hosts that are not iOS. iOS slices are not
  affected (`curl_setup.h` keeps `CURL_MACOS_CALL_COPYPROXIES` off for
  `TARGET_OS_IPHONE`).
- `src/carrier/http_client.c` compiles with deprecation warnings
  (`CURLOPT_HTTPPOST`, int passed where curl wants `long`); behaviour is
  unchanged and the two-node test passes. Worth cleaning up separately.

Only macos-arm64 was rebuilt and tested with the new pair. The iOS and
Android slices use the same `deps/*/CMakeLists.txt`; rebuild them with the
recipes in `docs/REBUILD-IOS-ANDROID.md` and re-run the app smoke tests
before shipping.

## Dead download URLs (history)

The CMake `ExternalProject` steps under `deps/` used to fetch their sources
from `https://github.com/elastos/...`. Three of those URLs now return **404**.
LibreSSL and curl were moved to their upstream sites (above); toxcore still
points at the dead elastos URL, so a build on a machine without a warm cache
still needs that one tarball copied in by hand:

| Tarball | Size (bytes) | SHA-256 |
|---|---|---|
| `libressl-2.9.2.tar.gz` (no longer used) | 1761077 | `27888e600cf61395d4129692afe2403854a2324d3c26c609d93490fde40ea253` |
| `c-toxcore-0.2.12.carrier.tar.gz` | 501481 | `9a6276d8c4e5202b972d45819fd0b47ff9b55750c8f35a83d54f3ca4a4726381` |
| `curl-7.64.0.tar.bz2` (no longer used) | 3012077 | `d573ba1c2d1cf9d8533fadcce480d778417964e8d04ccddcc76e591d544cf2eb` |

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
