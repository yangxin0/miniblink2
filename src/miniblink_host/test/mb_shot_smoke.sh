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
<input id="pv" value="preset"><input id="cb" type="checkbox" checked>
<button id="btn" onclick="document.getElementById('cr').textContent='clicked';">b</button>
<div id="cr">no</div>
<input id="sq" value=""><button id="go" type="button" onclick="render()">go</button>
<div id="results"></div>
<script>
localStorage.setItem('auth','tok-99');
sessionStorage.setItem('cart','3 items');
document.getElementById('kq').addEventListener('keydown',function(e){
  document.getElementById('krec').textContent=e.key;});
setTimeout(function(){window.delayedReady=true;},150);  // for --wait-eval
function render(){  // a search form that renders result rows on submit
  var q=document.getElementById('sq').value, box=document.getElementById('results');
  box.innerHTML='';
  for(var i=1;i<=3;i++){var d=document.createElement('div');d.className='res';
    d.textContent=q+'-'+i;box.appendChild(d);}
}
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
# checkre <name> <ERE> <actual> : extended-regex match
checkre() {
  if printf '%s' "$3" | grep -Eq "$2"; then PASS=$((PASS+1)); echo "  [PASS] $1";
  else FAIL=$((FAIL+1)); echo "  [FAIL] $1: [$3] !~ /$2/"; fi
}

URL="file://$FIX"

run 40 "$URL" "$PNG" --title;                 check "--title" "Shot Smoke" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --count "li.row";        check "--count" "3" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --attr "a" "href";       check "--attr href" "https://example.com/x" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --text;                  checkc "--text" "hello world" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --eval "1+2";            check "--eval" "3" "$(cat "$TMP/out")"
# --eval-json: structured extraction — an array/object comes out as real JSON
run 40 "$URL" "$PNG" --eval-json "[].map.call(document.querySelectorAll('li.row'),function(e){return e.textContent;})"
check "--eval-json (li.row array)" '["a","b","c"]' "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --eval-json "({n:document.querySelectorAll('li.row').length})"
check "--eval-json (object)" '{"n":3}' "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --visible "#msg";        check "--visible" "1" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --local-storage "auth";  check "--local-storage" "tok-99" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --session-storage "cart"; check "--session-storage" "3 items" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --url;                   checkc "--url" "fixture.html" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --value "#pv";           check "--value" "preset" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --checked "#cb";         check "--checked" "1" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --style "#msg" "display"; check "--style" "block" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --html;                  checkc "--html" "hello world" "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --html-for "#msg";       check "--html-for" '<p id="msg">hello world</p>' "$(cat "$TMP/out")"
run 40 "$URL" "$PNG" --rect "#msg";           checkre "--rect" '^[0-9]+,[0-9]+,[0-9]+,[0-9]+$' "$(cat "$TMP/out")"
# --click: a button's onclick mutates #cr; read it back via --eval
run 40 "$URL" "$PNG" --click "#btn" --eval "document.getElementById('cr').textContent"
check "--click fires handler" "clicked" "$(cat "$TMP/out")"

# cookie round-trip: inject then read back the same origin from the in-memory jar
run 40 "$URL" "$PNG" --set-cookie "https://t.test/" "s=1; Path=/" --cookies "https://t.test/"
check "--cookies round-trip" "s=1" "$(cat "$TMP/out")"

# --press: fill focuses #kq, then a trusted key event fires its keydown handler,
# which records e.key into #krec (read back via --eval)
run 40 "$URL" "$PNG" --fill "#kq" "x" --press "ArrowDown" \
    --eval "document.getElementById('krec').textContent"
check "--press delivers the key" "ArrowDown" "$(cat "$TMP/out")"

# --wait-eval: block until a deferred (150ms setTimeout) flag is truthy, then read
# it. Proves the wait actually blocked — the page settles before the timer fires.
run 40 "$URL" "$PNG" --wait-eval "window.delayedReady===true" \
    --eval "String(window.delayedReady===true)"
check "--wait-eval blocks until truthy" "true" "$(cat "$TMP/out")"

# integration: the canonical scrape workflow — fill a search box, click submit,
# wait for the rendered result rows, then extract them as structured JSON. This
# exercises the phase pipeline (interact -> synchronize -> extract) end to end, so
# a regression in flag ordering/composition (not just one flag) is caught.
run 40 "$URL" "$PNG" --fill "#sq" "apple" --click "#go" --wait-selector ".res" \
    --eval "JSON.stringify(Array.prototype.map.call(document.querySelectorAll('.res'),function(e){return e.textContent;}))"
check "integration: fill->click->wait->extract JSON" '["apple-1","apple-2","apple-3"]' "$(cat "$TMP/out")"

# --mobile: preset a phone viewport (390) + DPR 3 + iPhone UA in one flag
run 40 "$URL" "$PNG" --mobile \
    --eval "window.innerWidth+','+window.devicePixelRatio+','+/iPhone/.test(navigator.userAgent)"
check "--mobile preset (390/DPR3/iPhone UA)" "390,3,true" "$(cat "$TMP/out")"

# --mobile + --full compose: a full-page mobile screenshot must keep the mobile
# width (390), capture the full height, and apply the mobile media-query rule.
TALL="$TMP/tall.html"
cat > "$TALL" <<'HTML'
<!doctype html><html><head><style>body{margin:0}#m{height:40px;background:red}
@media (max-width:500px){#m{background:lime}}</style></head>
<body><div style="height:1500px"></div><div id="m"></div></body></html>
HTML
run 40 "file://$TALL" "$PNG" --mobile --full \
    --eval "window.innerWidth+'|'+(document.body.scrollHeight>1000)+'|'+getComputedStyle(document.getElementById('m')).backgroundColor"
check "--mobile + --full compose" "390|true|rgb(0, 255, 0)" "$(cat "$TMP/out")"

# --require: assert a scrape target is present; exit 3 when it isn't (for scripting)
run 40 "$URL" "$PNG" --require "#msg";          check "--require present -> exit 0" "0" "$RC"
run 40 "$URL" "$PNG" --require ".nonexistent";  check "--require absent -> exit 3" "3" "$RC"

# a missing local input file must fail (exit 1), not silently emit a blank PNG —
# both the bare-path and the file:// URL forms (consistent failure contract)
run 40 "$TMP/does-not-exist.html" "$PNG"; check "missing bare-path input -> exit 1" "1" "$RC"
checkc "missing input file message" "cannot open input file" "$(cat "$TMP/err")"
run 40 "file://$TMP/does-not-exist.html" "$PNG"; check "missing file:// input -> exit 1" "1" "$RC"

# bad-size guard: a non-numeric width positional must fail fast (exit 2), not crash
run 40 "$URL" --out "$PNG"; check "bad-size guard exit code" "2" "$RC"
checkc "bad-size guard message" "must be positive" "$(cat "$TMP/err")"

echo "mb_shot_smoke: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
