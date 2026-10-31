#!/bin/bash
# Headless browser tests of build/web (GitLab #36, docs/web.md): picker, server ROM, high refresh.
# Needs node with puppeteer-core (NODE_PATH) and CHROME=<chrome or chrome-headless-shell>, and the
# ROM at rom/baserom.md (given to the page through its file input / served beside a copy of the site).
# Results: runs/web/{picker,server,hifps}.json and PNGs.
set -u
cd "$(dirname "$0")/.."
[ -n "${CHROME:-}" ] || { echo "web_test.sh: set CHROME" >&2; exit 2; }
[ -f build/web/index.html ] || { echo "web_test.sh: build/web missing (tools/web_build.sh)" >&2; exit 2; }
rm -rf runs/web runs/site-server
mkdir -p runs/web
# the server-ROM deployment: a copy of the site with rom/baserom.md beside index.html
cp -r build/web runs/site-server
mkdir runs/site-server/rom
cp rom/baserom.md runs/site-server/rom/
(cd build/web && exec python3 -m http.server 8765 >/dev/null 2>&1) &
s1=$!
(cd runs/site-server && exec python3 -m http.server 8766 >/dev/null 2>&1) &
s2=$!
trap 'kill $s1 $s2 2>/dev/null' EXIT
sleep 1
rc=0
node tools/web_test.js http://localhost:8765/ picker runs/web/picker "$PWD/rom/baserom.md" > runs/web/picker.json 2>&1 || rc=1
node tools/web_test.js http://localhost:8766/ server runs/web/server > runs/web/server.json 2>&1 || rc=1
HIFPS=1 DPR=2 node tools/web_test.js http://localhost:8765/ picker runs/web/hifps "$PWD/rom/baserom.md" > runs/web/hifps.json 2>&1 || rc=1
rm -rf runs/site-server
for m in picker server hifps; do
  grep -q '"errors": \[\]' runs/web/$m.json && echo "$m: PASS" || { echo "$m: FAIL"; tail -5 runs/web/$m.json; }
done
echo "scooby.wasm $(stat -c %s build/web/scooby.wasm) bytes ($(gzip -9 -c build/web/scooby.wasm | wc -c) gzipped)," \
     "scooby.js $(stat -c %s build/web/scooby.js) bytes ($(gzip -9 -c build/web/scooby.js | wc -c) gzipped)"
exit $rc
