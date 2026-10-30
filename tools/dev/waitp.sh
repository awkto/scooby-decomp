#!/bin/bash
# waitp.sh LOG PATTERN [secs]
cd "$(dirname "$0")/../.."
timeout ${3:-100} bash -c "until grep -q -E '$2' runs/$1; do sleep 4; done"
grep -c plan: runs/$1; grep -v "plan:" runs/$1 | tail -5 | cut -c1-250
