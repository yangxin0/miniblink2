#!/usr/bin/env bash
# mb_shot_smoke.sh — CLI regression test for the mb_shot binary.
#
# The C++ smoke suites (mb_smoke / wke_smoke) cover the library/ABI, but the
# mb_shot command-line tool — its argument parsing and stdout extraction format
# — had no automated coverage; the extraction flags were only verified by hand.
# This script exercises them end-to-end against a local fixture (offline, no
# network) and asserts exact stdout, so a parsing typo or output-format drift
# fails the build.
#
# Usage: mb_shot_smoke.sh /path/to/chromium/out/Release
#   (the dir holding the mb_shot binary + libminiblink_host.dylib)

set -u
OUT_DIR="${1:?usage: mb_shot_smoke.sh /path/to/out/Release}"
BIN="$OUT_DIR/mb_shot"
[ -x "$BIN" ] || { echo "mb_shot_smoke: no mb_shot at $BIN" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PNG="$TMP/shot.png"
FIX="$TMP/fixture.html"
cat > "$FIX" <<'HTML'
<!doctype html><html><head><title>Shot Smoke</title></head><body>
<ul><li class="row">a</li><li class="row">b</li><li class="row">c</li></ul>
<a id="lnk" href="https://example.com/x">link</a>
<p id="msg">hello world</p>
<input id="kq" type="text"><div id="krec">nokey</div>
<script>
localStorage.setItem('auth','tok-99');
sessionStorage.setItem('cart','3 items');
document.getElementById('kq').addEventListener('keydown',function(e){
  document.getElementById('krec').textContent=e.key;});
</script></body></html>
HTML

PASS=0; FAIL=0
# run <timeout_s> <args...> : runs mb_shot under a watchdog; sets $RC, stdout in $TMP/out
run() {
  local t="$1"; shift
  DYLD_LIBRARY_PATH="$OUT_DIR" "$BIN" "$@" >"$TMP/out" 2>"$TMP/err" &
  local pid=$!
  ( sleep "$t"; kill -9 "$pid" 2>/dev/null ) & local wd=$!
  disown "$wd" 2>/dev/null || true  # keep the watchdog out of the job table (no SIGKILL notices)
  wait "$pid" 2>/dev/null; RC=$?
  kill -9 "$wd" 2>/dev/null
}
# check <name> <expected> <actual>
check() {
  if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "  [PASS] $1";
  else FAIL=$((FAIL+1)); echo "  [FAIL] $1: expected [$2] got [$3]"; fi
}
# checkc <name> <needle> <haystack> : substring containment
checkc() {
  case "$3" in *"$2"*) PASS=$((PASS+1)); echo "  [PASS] $1";;
  *) FAIL=$((FAIL+1)); echo "  [FAIL] $1: [$3] missing [$2]";; esac
}

URL="file://$FIX"

run 40 "$URL" "$PNG" --title;                 check "--title" "Shot Smoke" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --count "li.row";        check "--count" "3" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --attr "a" "href";       check "--attr href" "https://example.com/x" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --text;                  checkc "--text" "hello world" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --eval "1+2";            check "--eval" "3" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --visible "#msg";        check "--visible" "1" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --local-storage "auth";  check "--local-storage" "tok-99" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --session-storage "cart"; check "--session-storage" "3 items" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --url;                   checkc "--url" "fixture.html" "$(cat "$TMP/out")"

# cookie round-trip: inject then read back the same origin from the in-memory jar
run 40 "$URL" "$PNG" --set-cookie "https://t.test/" "s=1; Path=/" --cookies "https://t.test/"
check "--cookies round-trip" "s=1" "$(cat "$TMP/out")"

# --press: fill focuses #kq, then a trusted key event fires its keydown handler,
# which records e.key into #krec (read back via --eval)
run 40 "$URL" "$PNG" --fill "#kq" "x" --press "ArrowDown" \
    --eval "document.getElementById('krec').textContent"
check "--press delivers the key" "ArrowDown" "$(cat "$TMP/out")"

# bad-size guard: a non-numeric width positional must fail fast (exit 2), not crash
run 40 "$URL" --out "$PNG"; check "bad-size guard exit code" "2" "$RC"
checkc "bad-size guard message" "must be positive" "$(cat "$TMP/err")"

echo "mb_shot_smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
