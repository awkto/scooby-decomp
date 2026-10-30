#!/bin/bash
# test_replay.sh  (on the VM, worktree root; needs runs/states/pw_w0_r16_init.state from `campaign.py warp`)
# Records a 3000-frame random-input segment with `record`, replays it from the same savestate with `replay`
# and checks that the resulting 64 KiB RAM dumps are identical.
o=runs/replay_test
rm -rf $o; mkdir -p $o
cp runs/states/pw_w0_r16_init.state $o/start.state
tools/harness/h.sh tools/scripts/test_replay.txt $o 2>&1 | tail -1
cat $o/peek.log
cmp $o/a.ram $o/b.ram && echo "replay OK: RAM dumps identical"
