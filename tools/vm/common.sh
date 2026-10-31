# shared by tools/vm/* — sourced
# Helper scripts for running heavy builds/tests on a pool of remote Linux machines (any provider).
# State lives in $SCOOBY_VM_HOME (default: <main checkout>/.vm, untracked) so all worktrees share it:
#   vms.txt = "name ip" lines (one per machine; ssh as root), leases/ = lock dirs.
_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCOOBY_REPO=${SCOOBY_REPO:-$(dirname "$(git -C "$_here" rev-parse --path-format=absolute --git-common-dir)")}
VM_HOME=${SCOOBY_VM_HOME:-$SCOOBY_REPO/.vm}
VM_LIST=$VM_HOME/vms.txt                             # "name ip" lines
LEASES=$VM_HOME/leases
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$VM_HOME/known_hosts -o ConnectTimeout=10 -o ServerAliveInterval=30)
mkdir -p "$LEASES"
vm_ip() { awk -v n="$1" '$1==n{print $2}' "$VM_LIST"; }
die() { echo "$*" >&2; exit 1; }
