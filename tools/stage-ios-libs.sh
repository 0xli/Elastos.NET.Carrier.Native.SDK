#!/bin/bash
# Stage the native iOS static libraries into the Swift SDK's NativeDistributions,
# the layout ElastosCarrierSDK.xcodeproj links against:
#   NativeDistributions/lib/-iphoneos         arm64 device slices
#   NativeDistributions/lib/-iphonesimulator  fat arm64+x86_64 simulator slices
#
# Versioned copy of ~/devs/agentnet-v2/stage-ios-libs.sh (2026-09-18).
#   NATIVE_BUILD  this repo's build dir       (default: <repo>/build)
#   SWIFT_SDK     Elastos.CarrierClassic.Swift checkout (default: ~/devs/agentnet-v2/swift)
# See docs/REBUILD-IOS-ANDROID.md.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
N=${NATIVE_BUILD:-$HERE/build}
S=${SWIFT_SDK:-$HOME/devs/agentnet-v2/swift}/NativeDistributions
LIBS="libcarrier.a libcarriersession.a libcarrierfiletrans.a libcjson.a libcrypto.a libcrystal.a libcurl.a libflatccrt.a libpj.a libpjlib-util.a libpjmedia.a libpjnath.a libsodium.a libsrtp.a libssl.a libtls.a libtoxcore.a libz.a"
find_lib() { # $1 build dir, $2 lib name
  for p in "$1/src/carrier/$2" "$1/src/session/$2" "$1/src/filetransfer/$2" "$1/intermediates/lib/$2"; do [ -f "$p" ] && { echo "$p"; return; }; done
  echo "MISSING $2 in $1" >&2; return 1
}
rm -rf "$S/lib/-iphoneos" "$S/lib/-iphonesimulator"; mkdir -p "$S/lib/-iphoneos" "$S/lib/-iphonesimulator" "$S/include"
for l in $LIBS; do
  cp "$(find_lib $N/ios-iphoneos-arm64 $l)" "$S/lib/-iphoneos/$l"
  a=$(find_lib $N/ios-iphonesimulator-arm64 $l); x=$(find_lib $N/ios-iphonesimulator-x86_64 $l)
  lipo -create "$a" "$x" -output "$S/lib/-iphonesimulator/$l"
done
# carrier.h as the Swift bindings see it: without the deprecated-API include,
# which the framework build cannot resolve.
cp "$HERE/src/carrier/carrier.h" "$S/include/carrier.h"; sed -i "" "/#include <carrier_deprecated.h>/d" "$S/include/carrier.h"; rm -f "$S/include/carrier_deprecated.h"
echo "staged:"; ls "$S/lib/-iphoneos" | wc -l; ls "$S/lib/-iphonesimulator" | wc -l; lipo -info "$S/lib/-iphonesimulator/libcarrier.a"
