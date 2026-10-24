# Rebuilding the iOS xcframework and the Android artifact from this tree

Written 2026-10-24 for the Express backup-node work (two independent Express
nodes; `src/carrier/express.c` pulls from every node). Every path below is the
one on Weili's mbp14; adjust `$HOME` paths on another machine.

| piece | checkout | branch |
|---|---|---|
| C core (this repo) | `~/devs/agentnet-v2/native` (worktree of `~/blockchain/eth/Elastos.NET.Carrier.Native.SDK`) | `feat/agentnet-proto-v2` → `0xli/feat/agentnet-proto-v2` |
| Swift bindings | `~/devs/agentnet-v2/swift` (`0xli/Elastos.CarrierClassic.Swift`) | `feat/agentnet-proto-v2` |
| iOS app | `~/StudioProjects/Elastos.NET.WebRTC.Swift.SDK-0xli`, consumes `Packages/CarrierSDK/ElastosCarrierSDK.xcframework` | see its `docs/RELEASE-AND-CI.md` §6.9 |
| Android Java/JNI | `~/devs/agentnet-v2/android` (worktree of `~/StudioProjects/Elastos.NET.Carrier.Android.SDK`) | `feat/agentnet-proto-v2` (`agent/permissionless-realtime-network` is the parent) |
| Android app | `~/StudioProjects/beaglechat`, consumes `org.elastos:carrier:<version>` from its `local-maven/` | `app/build.gradle` line ~258 |

`~/blockchain/eth/Elastos.NET.Carrier.iOS.SDK` (master) is the old Swift
checkout; the one that ships is `~/devs/agentnet-v2/swift`.

## 0. Before any slice: dependency tarballs

`deps/*` download into `build/.tarballs/` (shared by every build dir). Two of
the upstream URLs are live again after the 2026-10-24 bump, one is dead:

- `libressl-4.3.4.tar.gz`, `curl-8.23.0.tar.bz2`: downloaded by CMake from
  ftp.openbsd.org / curl.se (hashes pinned in `deps/*/CMakeLists.txt`).
- `c-toxcore-0.2.12.carrier.tar.gz`: dead URL, copy it from an older
  `build/.tarballs/` (see `docs/BUILD-DEPS.md` for size and SHA-256).

A failed download leaves a 0-byte file that later configures treat as cached;
delete any zero-length file in `build/.tarballs/` before retrying.

The deps changed on 2026-10-24 (LibreSSL 2.9.2 → 4.3.4, curl 7.64.0 →
8.23.0). A build dir that predates that must have its old
`deps/libressl/external` and `deps/libcurl/external` removed, or start from a
fresh build dir, so the new tarballs are actually built.

## 1. macOS (host build, runs the tests)

```bash
cd ~/devs/agentnet-v2/native && cmake -S . -B build/macos-arm64 -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=ON -DENABLE_STATIC=ON -DENABLE_APPS=ON -DENABLE_TESTS=ON && make -C build/macos-arm64 -j8
```

Then the two-node Express test (needs the Express repo venv; details in
`tests/express-multinode/README.md`):

```bash
~/StudioProjects/Elastos.CarrierClassic.Express/venv/bin/python tests/express-multinode/run_test.py --cert-type ecdsa
```

## 2. iOS xcframework

### 2.1 C core, three slices (10–20 min each the first time; deps are rebuilt per slice)

```bash
cd ~/devs/agentnet-v2/native && for cfg in "iphoneos arm64" "iphonesimulator arm64" "iphonesimulator x86_64"; do plat=${cfg%% *}; arch=${cfg##* }; d=build/ios-$plat-$arch; mkdir -p $d; (cd $d && cmake -DCMAKE_TOOLCHAIN_FILE=../../cmake/iOSToolchain2.cmake -DIOS_PLATFORM=$plat -DIOS_ARCH=$arch -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_APPS=OFF -DENABLE_TESTS=OFF -DCMAKE_BUILD_TYPE=Release ../.. && make -j8) || break; done
```

zsh note: `set -- $cfg` does not split words in zsh; the `${cfg%% *}` /
`${cfg##* }` form above works in both shells.

Outputs per slice: `build/ios-<plat>-<arch>/src/{carrier,session,filetransfer}/lib*.a`
plus the deps in `build/ios-<plat>-<arch>/intermediates/lib/`.

### 2.2 Stage the 18 static libraries into the Swift SDK

```bash
~/devs/agentnet-v2/native/tools/stage-ios-libs.sh
```

(`NATIVE_BUILD` and `SWIFT_SDK` override the defaults.) It copies the device
slices to `NativeDistributions/lib/-iphoneos`, `lipo`s arm64+x86_64 simulator
slices into `NativeDistributions/lib/-iphonesimulator`, and copies `carrier.h`
into `NativeDistributions/include/` with the `#include <carrier_deprecated.h>`
line removed (the framework does not build with it). New C API needs a
matching `@_silgen_name` declaration and struct mirror in
`NativeDistributions/include/CCarrier.swift` (char arrays are `Int8` tuples
of exactly the `carrier.h` length). C symbols not showing in `nm` of the
framework is normal.

### 2.3 Swift framework, both SDKs, then the xcframework

Key points: no code signing; the simulator build must explicitly re-allow
arm64 (the project's `EXCLUDED_ARCHS` removes it).

```bash
cd ~/devs/agentnet-v2/swift && OUT=~/devs/agentnet-v2/xcf && rm -rf $OUT && xcodebuild -project ElastosCarrierSDK.xcodeproj -target ElastosCarrierSDK -configuration Release -sdk iphoneos ARCHS=arm64 ONLY_ACTIVE_ARCH=NO BUILD_LIBRARY_FOR_DISTRIBUTION=YES SKIP_INSTALL=NO IPHONEOS_DEPLOYMENT_TARGET=13.0 CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CONFIGURATION_BUILD_DIR=$OUT/iphoneos build
```

```bash
cd ~/devs/agentnet-v2/swift && OUT=~/devs/agentnet-v2/xcf && xcodebuild -project ElastosCarrierSDK.xcodeproj -target ElastosCarrierSDK -configuration Release -sdk iphonesimulator ARCHS="arm64 x86_64" VALID_ARCHS="arm64 x86_64" EXCLUDED_ARCHS="" ONLY_ACTIVE_ARCH=NO BUILD_LIBRARY_FOR_DISTRIBUTION=YES SKIP_INSTALL=NO IPHONEOS_DEPLOYMENT_TARGET=13.0 CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" CONFIGURATION_BUILD_DIR=$OUT/iphonesimulator build
```

```bash
OUT=~/devs/agentnet-v2/xcf && xcodebuild -create-xcframework -framework $OUT/iphoneos/ElastosCarrierSDK.framework -framework $OUT/iphonesimulator/ElastosCarrierSDK.framework -output $OUT/ElastosCarrierSDK.xcframework
```

### 2.4 Into the app

```bash
rm -rf ~/StudioProjects/Elastos.NET.WebRTC.Swift.SDK-0xli/Packages/CarrierSDK/ElastosCarrierSDK.xcframework && cp -R ~/devs/agentnet-v2/xcf/ElastosCarrierSDK.xcframework ~/StudioProjects/Elastos.NET.WebRTC.Swift.SDK-0xli/Packages/CarrierSDK/
```

Then the app's build-for-testing + unit tests, and the second Express node in
`bootstrap.json` (`express.beagle.chat`, port 443, its public key) as the
SECOND `expressNodes` entry; lens stays first. The app recipe and its traps
(workspace not project, a booted simulator id, `xcode-select`, `NODE_BINARY`)
are in that repo's `docs/RELEASE-AND-CI.md` §6.9 and §6.11.

If `xcode-select -p` points at CommandLineTools, prefix the xcodebuild calls
with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.

## 3. Android artifact (`org.elastos:carrier:6.0.5-beagle1`)

Arm64 only, like `6.0.4-beagle4` (Play's 16 KB page-size gate; the other ABIs
would need every other prebuilt AAR rebuilt too, see beaglechat
`docs/HANDOVER_PLAY_16KB.md`). NDK **r28.2** (`28.2.13676358`) links 16 KB
aligned by default; verify at the end anyway.

### 3.1 C core, arm64-v8a, API 24

API 24 is required (`getifaddrs`). It used to be a local edit of
`cmake/AndroidToolchain.cmake`; since 2026-10-24 it is the `ANDROID_API`
option (default 21).

```bash
cd ~/devs/agentnet-v2/native && NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358 && mkdir -p build/android-arm64 && (cd build/android-arm64 && cmake -DCMAKE_TOOLCHAIN_FILE=../../cmake/AndroidToolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_NDK_HOME=$NDK -DANDROID_API=24 -DENABLE_SHARED=ON -DENABLE_STATIC=ON -DENABLE_APPS=OFF -DENABLE_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo ../.. && make -j8 && make install)
```

`make install` fills `build/android-arm64/outputs/{lib,include}` (the
cross-build install prefix); the iOS slices need no install step.

Trap, first build only: the first `make` creates the standalone toolchain
`build/android-arm64/.android_toolchain` with the NDK's
`make_standalone_toolchain.py`, which no longer provides the GNU-named
binutils the autotools deps (libsodium, pjproject, curl…) call. Add the
wrappers once, then re-run `make -j8`:

```bash
cd ~/devs/agentnet-v2/native/build/android-arm64/.android_toolchain/bin && L=$HOME/Library/Android/sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin && for t in ar ranlib strip nm; do printf '#!/bin/sh\nexec "%s/llvm-%s" "$@"\n' "$L" "$t" > aarch64-linux-android-$t; chmod +x aarch64-linux-android-$t; done
```

Outputs: `build/android-arm64/outputs/lib/{libcarrier,libcarriersession,libcarrierfiletrans,libcrystal}.so`
(and the `.a` files).

### 3.2 Headers and shared libs into the Android SDK tree

```bash
cd ~/devs/agentnet-v2 && cp native/build/android-arm64/outputs/lib/{libcarrier,libcarriersession,libcarrierfiletrans,libcrystal}.so android/app/native-dist/lib/arm64-v8a/ && cp native/build/android-arm64/outputs/include/*.h android/app/native-dist/include/ && ls android/app/native-dist/lib/arm64-v8a android/app/native-dist/include
```

`outputs/include` holds the same six headers `app/native-dist/include/`
carries (`carrier*.h`, `carrier_session*.h`, `carrier_filetransfer*.h`).

### 3.3 `libcarrierjni.so` with the NDK's own CMake toolchain

The repo's Gradle (AGP 3.3, bintray plugin) does not run on JDK 17, so the JNI
library is built directly. Validated 2026-10-24 (16 KB LOAD alignment out of
the box):

```bash
NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358 && A=$HOME/devs/agentnet-v2/android && rm -rf /tmp/carrierjni && cmake -S $A/app/src/main/cpp -B /tmp/carrierjni -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/carrierjni -j8 && ls -l /tmp/carrierjni/libcarrierjni.so
```

### 3.4 Assemble the AAR by hand

`classes.jar` holds `.class` files (not dex); the Java sources use only
`android.jar`. Version `6.0.5-beagle1` for this change.

```bash
V=6.0.5-beagle1 && NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358 && A=$HOME/devs/agentnet-v2/android/app && W=/tmp/carrier-aar && rm -rf $W && mkdir -p $W/classes $W/aar/jni/arm64-v8a && javac --release 11 -cp $HOME/Library/Android/sdk/platforms/android-35/android.jar -d $W/classes $(find $A/src/main/java -name '*.java') && (cd $W/classes && jar cf ../aar/classes.jar .) && cp $A/src/main/AndroidManifest.xml $W/aar/ && cp $A/native-dist/lib/arm64-v8a/*.so /tmp/carrierjni/libcarrierjni.so $NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so $W/aar/jni/arm64-v8a/ && (cd $W/aar && zip -qr ../carrier-$V.aar .) && unzip -l $W/carrier-$V.aar
```

Expected listing: `AndroidManifest.xml`, `classes.jar`, and
`jni/arm64-v8a/{libcrystal,libcarrier,libcarriersession,libcarrierfiletrans,libcarrierjni,libc++_shared}.so`
(what `6.0.4-beagle4` contains).

Verify the 16 KB alignment of every `.so` in the AAR (each `LOAD` line must
end in `0x4000`):

```bash
NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358 && for f in /tmp/carrier-aar/aar/jni/arm64-v8a/*.so; do echo "$f"; $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf -lW "$f" | grep LOAD | awk '{print $NF}' | sort -u; done
```

### 3.5 Publish to beaglechat's local maven and bump the app

```bash
V=6.0.5-beagle1 && M=$HOME/StudioProjects/beaglechat/local-maven/org/elastos/carrier/$V && mkdir -p $M && cp /tmp/carrier-aar/carrier-$V.aar $M/ && printf '<?xml version="1.0" encoding="UTF-8"?>\n<project xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 https://maven.apache.org/xsd/maven-4.0.0.xsd" xmlns="http://maven.apache.org/POM/4.0.0"\n    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">\n  <modelVersion>4.0.0</modelVersion>\n  <groupId>org.elastos</groupId>\n  <artifactId>carrier</artifactId>\n  <version>%s</version>\n  <packaging>aar</packaging>\n</project>\n' "$V" > $M/carrier-$V.pom && ls $M
```

Then in `~/StudioProjects/beaglechat/app/build.gradle`:
`implementation 'org.elastos:carrier:6.0.5-beagle1'`, add the second Express
node to `bgservers.json` as the SECOND entry, build, and smoke-test on a
device with lens stopped (a message sent while the receiver is offline must
arrive from `express.beagle.chat`).

## 4. What to re-check after the LibreSSL/curl bump

The deps CMake files are shared by all slices, but only macos-arm64 was
rebuilt and tested with LibreSSL 4.3.4 + curl 8.23.0 (`docs/BUILD-DEPS.md`).
On the first iOS and Android rebuild after it:

- iOS: curl's configure must pick `--with-openssl` against the slice's
  `intermediates`; the `SCDynamicStoreCopyProxies` link dependency does not
  apply to iOS (curl keeps it off for `TARGET_OS_IPHONE`), so the Swift
  framework needs no new framework link. If the link complains about
  `SystemConfiguration`, add that framework to the ElastosCarrierSDK target.
- Android: `--without-ca-bundle --without-ca-path` keeps configure from
  probing the host; nothing else platform-specific changed.
- Both: the offline-message round trip on a device, against both nodes.
