#!/bin/bash
# On the VM after `campaign.py natural`: checks that "boot + inputs" (continuous) equals "load savestate saved at the
# end of the boot + same inputs" in a new process.  (Known to FAIL partly, see states/INDEX.md notes.)
o=runs/det; rm -rf $o; mkdir -p $o
cp runs/states/nat_w0_r16.state $o/
tools/harness/h.sh tools/scripts/test_state_determinism.txt $o 2>&1 | tail -1
rm -rf runs/det2; mkdir runs/det2; cp runs/states/nat_w0_r16.state runs/det2/
tools/harness/h.sh tools/scripts/test_state_determinism2.txt runs/det2 2>&1 | tail -1
cmp $o/a.ram runs/det2/b.ram && echo "RAM identical" 
cmp -l $o/a.state runs/det2/b.state | wc -l
