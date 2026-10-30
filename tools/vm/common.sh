# shared by tools/vm/* — sourced
VM_HOME=/path/to/scooby-decomp/.vm      # outside git, shared by all worktrees
VM_LIST=$VM_HOME/vms.txt                             # "name ip" lines
LEASES=$VM_HOME/leases
SSH_OPTS=(-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$VM_HOME/known_hosts -o ConnectTimeout=10 -o ServerAliveInterval=30)
mkdir -p "$LEASES"
vm_ip() { awk -v n="$1" '$1==n{print $2}' "$VM_LIST"; }
die() { echo "$*" >&2; exit 1; }
