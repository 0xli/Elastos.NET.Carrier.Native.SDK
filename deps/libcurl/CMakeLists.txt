project(curl)

include(CarrierDefaults)
include(ExternalProject)

if(WIN32)
    set(PATCH_ARGS -s -p1 < "${CMAKE_CURRENT_LIST_DIR}/curl.patch")

    set(CONFIGURE_CMD "echo")
    set(CONFIGURE_ARGS "Done")

    set(BUILD_CMD cd winbuild && nmake)
    set(BUILD_ARGS
        -f Makefile.vc
        MODE=static
        "WITH_DEVEL=${CARRIER_INT_DIST_DIR}"
        WITH_SSL=static
        WITH_ZLIB=static
        ENABLE_WINSSL=no
        ENABLE_IDN=no)

    set(OUTPUT_PATH builds\\libcurl)
    set(INSTALL_CMD
        xcopy "${OUTPUT_PATH}\\include\\curl" "${CARRIER_INT_DIST_DIR}\\include\\curl" /e /i /h /y &&
        copy "${OUTPUT_PATH}\\lib\\libcurl_a.lib" "${CARRIER_INT_DIST_DIR}\\lib\\curl.lib" /y)
else()
    include(ExternalConfigureArgs)

    set(PATCH_EXE "echo")
    set(PATCH_ARGS "Done")

    # curl 8.x: the TLS backend must be named (--with-openssl, which also
    # covers LibreSSL); --with-ssl and the *ssl/polarssl/cyassl/metalink
    # switches of the 7.64 era are gone. HTTP(S) only, no CA bundle path
    # baked in (the SDK sets CURLOPT_SSL_VERIFYPEER 0 and the iOS/Android
    # builds must not inherit a macOS path).
    set(CONFIGURE_CMD "./configure")
    set(CONFIGURE_ARGS
        "--prefix=${CARRIER_INT_DIST_DIR}"
        "--with-openssl=${CARRIER_INT_DIST_DIR}"
        "--with-zlib=${CARRIER_INT_DIST_DIR}"
        "--disable-shared"
        "--enable-static"
        "--without-ca-bundle"
        "--without-ca-path"
        "--without-brotli"
        "--without-zstd"
        "--without-ldap-lib"
        "--without-lber-lib"
        "--without-gnutls"
        "--without-mbedtls"
        "--without-wolfssl"
        "--without-libpsl"
        "--without-libssh2"
        "--without-libssh"
        "--without-libidn2"
        "--without-nghttp2"
        "--without-ngtcp2"
        "--without-nghttp3"
        "--without-libgsasl"
        "--enable-http"
        "--disable-ftp"
        "--disable-file"
        "--disable-ldap"
        "--disable-ldaps"
        "--disable-rtsp"
        "--disable-proxy"
        "--disable-dict"
        "--disable-telnet"
        "--disable-tftp"
        "--disable-pop3"
        "--disable-imap"
        "--disable-smtp"
        "--disable-gopher"
        "--disable-mqtt"
        "--disable-websockets"
        "--disable-manual"
        "--disable-docs"
        ${CONFIGURE_ARGS_INIT})

    set(INSTALL_CMD
        make install)
endif()

ExternalProject_Add(
    curl

    PREFIX ${CARRIER_DEPS_BUILD_PREFIX}
    # curl 8.23.0 (2026). Hash computed from the curl.se download on
    # 2026-10-24; mirror: https://github.com/curl/curl/releases/download/curl-8_23_0/
    URL "https://curl.se/download/curl-8.23.0.tar.bz2"
    URL_HASH SHA256=95b22eb32bf6d1fa6e3e9463bdb61cece3efc56d86576a9b3cbd428246a6d854
    DOWNLOAD_NAME "curl-8.23.0.tar.bz2"
    DOWNLOAD_DIR ${CARRIER_DEPS_TARBALL_DIR}
    DOWNLOAD_NO_PROGRESS 1
    LOG_DOWNLOAD FALSE

    BUILD_IN_SOURCE 1

    PATCH_COMMAND ${PATCH_EXE} ${PATCH_ARGS}
    CONFIGURE_COMMAND ${CONFIGURE_CMD} ${CONFIGURE_ARGS}
    BUILD_COMMAND ${BUILD_CMD} ${BUILD_ARGS}
    INSTALL_COMMAND ${INSTALL_CMD}
)

set(CURL_DEPS
    curl
    ssl
    crypto
    z)

if(WIN32)
    set(CURL_DEPS ${CURL_DEPS} Ws2_32 Iphlpapi)
endif()

if(NOT ANDROID)
    set(CURL_DEPS ${CURL_DEPS} pthread)
endif()

# curl 8 on macOS (not iOS: curl_setup.h keeps CURL_MACOS_CALL_COPYPROXIES
# off for TARGET_OS_IPHONE) calls SCDynamicStoreCopyProxies once at init.
if(APPLE AND NOT IOS)
    set(CURL_DEPS ${CURL_DEPS} "-framework CoreFoundation" "-framework SystemConfiguration")
endif()

add_library(libcurl INTERFACE)
target_link_libraries(libcurl INTERFACE ${CURL_DEPS})
if(WIN32)
    target_compile_definitions(libcurl INTERFACE CURL_STATICLIB)
endif()

if(ENABLE_STATIC)
    export_static_library(curl)
endif()
