#!/bin/bash
# Runs the engine tests and a protocol smoke test. Usage: tools/test.sh [path/to/sprfst]
set -e
cd "$(dirname "$0")/.."
SPRFST=${1:-$(command -v sprfst || echo build/bin/sprfst)}
export SPRFST_HOME="$PWD" SREON_RES="$PWD" SREON_HOME="$(mktemp -d)" SREON_SPRFST="$SPRFST"
echo "== type check"; "$SPRFST" check engine/main.spf
echo "== tests";      "$SPRFST" test engine/tests.spf
echo "== engine protocol"
printf '%s\n' '{"op":"boot","dark":true,"id":1}' '{"op":"page","url":"sreon://newtab","id":2}' \
  '{"op":"page","url":"sreon://studio?tab=guide","id":3}' '{"op":"rules","id":4}' \
  '{"op":"nav","url":"http://example.com/?utm_source=a","id":5}' '{"op":"quit","id":6}' \
  | "$SPRFST" run engine/main.spf -- --serve | python3 -c '
import sys, json
rows = [json.loads(l) for l in sys.stdin if l.strip()]
by = {r.get("id"): r for r in rows}
assert by[1]["ok"], "boot"
assert "Sreon" in by[2]["body"] and "Powered by" in by[2]["body"], "new tab"
assert "guidebook" in by[3]["body"].lower(), "studio"
assert len(by[4]["lists"]) >= 5, "rules"
assert by[5]["allow"] is False and by[5]["load"].startswith("https://"), "nav upgrade"
print("protocol ok")'
echo "All good."
