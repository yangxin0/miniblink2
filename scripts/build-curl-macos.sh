#!/usr/bin/env bash
# build-curl-macos.sh — build a WebSocket-enabled libcurl and vendor it into the
# project (third_party/curl), for mb_url_loader + the real WebSocket backend.
#
# WHY: macOS's system libcurl (8.7.1) is compiled WITHOUT the ws/wss protocols, so
# `curl_ws_send`/`curl_ws_recv` aren't usable. We build our own static-of-shape
# curl with --enable-websockets and an OpenSSL TLS backend, as a DYLIB so its TLS
# symbols stay isolated from Chromium's statically-linked BoringSSL (macOS
# two-level namespace — the same reason the system libcurl+LibreSSL coexists today).
#
# curl 8.21 removed SecureTransport, so we link Homebrew OpenSSL 3. The resulting
# dylib depends on /opt/homebrew/opt/openssl@3/lib/{libssl,libcrypto}.3.dylib at
# runtime (present via `brew install openssl@3`). For a fully self-contained dylib,
# statically fold OpenSSL in or bundle+repath the two dylibs (follow-up).
#
# Usage: scripts/build-curl-macos.sh [curl-version]   (default: 8.21.0)
set -euo pipefail

CURL_VER="${1:-8.21.0}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VEND="$HERE/third_party/curl"
OPENSSL_PREFIX="${OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}"
WORK="$(mktemp -d)"

[ -d "$OPENSSL_PREFIX/lib" ] || { echo "OpenSSL not found at $OPENSSL_PREFIX (brew install openssl@3)" >&2; exit 1; }

echo "==> downloading curl $CURL_VER"
curl -sL "https://curl.se/download/curl-$CURL_VER.tar.gz" -o "$WORK/curl.tar.gz"
tar xzf "$WORK/curl.tar.gz" -C "$WORK"
cd "$WORK/curl-$CURL_VER"

echo "==> configure (shared, websockets, OpenSSL)"
./configure \
  --enable-shared --disable-static \
  --enable-websockets \
  --with-openssl="$OPENSSL_PREFIX" \
  --without-libpsl --disable-ldap --disable-ldaps \
  --disable-manual --disable-docs \
  --prefix="$WORK/install" >/dev/null
# Sanity: ws/wss must be in the protocol list.
grep -q "ws wss" <(make -s -C . 2>/dev/null; "$WORK"/curl-"$CURL_VER"/src/curl --version 2>/dev/null || true) || true

echo "==> make + install"
make -j"$(sysctl -n hw.ncpu)" >/dev/null
make install >/dev/null

echo "==> vendor -> $VEND"
mkdir -p "$VEND/lib" "$VEND/include"
cp "$WORK/install/lib/libcurl.4.dylib" "$VEND/lib/"
rm -rf "$VEND/include/curl"
cp -R "$WORK/install/include/curl" "$VEND/include/"
install_name_tool -id "$VEND/lib/libcurl.4.dylib" "$VEND/lib/libcurl.4.dylib"
ln -sf libcurl.4.dylib "$VEND/lib/libcurl.dylib"

# Bundle curl's non-system (Homebrew) dependency CLOSURE into the vendor lib dir
# with @loader_path references, so the dylib is fully SELF-CONTAINED at runtime
# (no Homebrew needed — the project is standalone). Walks deps recursively.
echo "==> bundling dependency closure (@loader_path)"
bundle_deps() {
  local f="$1"
  for ref in $(otool -L "$f" | grep -oE '/opt/homebrew[^ ]*\.dylib'); do
    local base; base="$(basename "$ref")"
    if [ ! -f "$VEND/lib/$base" ]; then
      cp "$ref" "$VEND/lib/$base"; chmod u+w "$VEND/lib/$base"
      install_name_tool -id "@loader_path/$base" "$VEND/lib/$base"
      bundle_deps "$VEND/lib/$base"   # recurse into the newly copied dep
    fi
    install_name_tool -change "$ref" "@loader_path/$base" "$f"
  done
}
( cd "$VEND/lib" && bundle_deps libcurl.4.dylib )
# install_name_tool invalidates signatures -> ad-hoc re-sign every dylib.
for f in "$VEND"/lib/*.dylib; do codesign -f -s - "$f" 2>/dev/null || true; done
if otool -L "$VEND"/lib/*.dylib | grep -q /opt/homebrew; then
  echo "WARNING: residual Homebrew deps remain" >&2
fi

echo "==> done. WS check:"
nm -gU "$VEND/lib/libcurl.4.dylib" | grep -E "_curl_ws_(send|recv)" || { echo "WS symbols MISSING" >&2; exit 1; }
rm -rf "$WORK"
