#!/usr/bin/env bash
# Power-cut soak for boot-restore: cut the power N times and prove the
# box always comes back into its committed configuration.
#
# This is the VM-tier stand-in for ROADMAP Phase 0's exit gate ("pull
# power mid-soak on the bench switch 50 times"). A `virsh destroy` is a
# hard power-off with no clean shutdown, so it exercises exactly what
# matters: the durable confd store survives an unclean stop, and the
# boot unit re-applies it. The bench version of this script pulls a
# relay instead of calling virsh; everything after the cut is the same.
#
# Requires: the target running with einheit-s5-boot.service ENABLED (a
# power cut leaves no chance to run --apply-boot by hand), passwordless
# sudo on the target, and libvirt access on the host.
# NEVER run against a production switch.
#
# GOTCHA when debugging a failure here: `journalctl -u einheit-s5-boot`
# will be missing most cycles. journald does not fsync per message
# either, so a hard power-off ~20s into a boot loses its own log along
# with everything else in the page cache — only the longest-lived boots
# survive in the journal. Assert on the box's state, not on the log.
#
# Usage: test/power_cycle.sh [ssh-host] [cycles] [vm-name]
set -u
HOST=${1:-s5-test}
CYCLES=${2:-10}
VM=${3:-$HOST}
LIBVIRT=${LIBVIRT_URI:-qemu:///system}
PASS=0
FAIL=0

ok() {
  PASS=$((PASS + 1))
  echo "  PASS  $1"
}

bad() {
  FAIL=$((FAIL + 1))
  echo "  FAIL  $1"
  [ -n "${2:-}" ] && echo "        got: $2"
}

# assert_in <desc> <needle> <haystack>
assert_in() {
  case "$3" in
    *"$2"*) ok "$1" ;;
    *) bad "$1" "$(echo "$3" | head -1)" ;;
  esac
}

box() {
  ssh -o ConnectTimeout=5 "$HOST" "$1" 2>/dev/null
}

wait_for_ssh() {
  local i
  for i in $(seq 1 60); do
    if ssh -o ConnectTimeout=3 -o BatchMode=yes "$HOST" true 2>/dev/null; then
      return 0
    fi
    sleep 2
  done
  return 1
}

# The boot unit is ordered before sshd, so an answering ssh should mean
# it is done — measured at 0-1s of slack on an idle host. Wait anyway:
# this script also has to work on an init system with no such ordering,
# and asserting on a half-built fabric produces a confusing failure that
# looks like a product bug.
# Waits for a TERMINAL state and echoes it, so the caller can tell the
# three outcomes apart. Reporting them as one thing is how a "the unit
# never ran" boot spent an afternoon being misdiagnosed as a hang:
#   active     — ran and succeeded (RemainAfterExit keeps it active)
#   failed     — ran and failed
#   inactive   — never ran on this boot, which is a different bug
# The unit's own budget is TimeoutStartSec=120, so waiting less than
# that would flag a slow-but-legal restore as a failure.
wait_for_boot_unit() {
  local i state
  for i in $(seq 1 120); do
    state=$(box 'systemctl show -p ActiveState --value einheit-s5-boot')
    case "$state" in
      active | failed) echo "$state"; return 0 ;;
    esac
    sleep 1
  done
  echo "${state:-unreachable}"
  return 1
}

# A configuration distinctive enough that a factory-default box cannot
# pass by accident, and covering every config FAMILY: identity, port
# admin state, VLAN membership, link parameters, the MAC table,
# multicast snooping, spanning tree, an SVI address, a static route and
# a DHCP pool. A new family that does not survive a power cut is no more
# shipped than one that was never written — and the Phase 2 families are
# the interesting ones here, because each of them is a daemon that has
# to be brought back up from a generated file that no longer exists
# after the cut (/var/run is a tmpfs).
echo "== commit the configuration under test"
printf '%b' 'configure\nset hostname power-soak\nset ports.lan3.enabled false\nset ports.lan1.vlan.77 untagged-pvid\nset ports.lan1.mtu 9000\nset mac.aging_time 600\nset mac.static.aa:bb:cc:dd:ee:01.port lan1\nset mac.static.aa:bb:cc:dd:ee:01.vlan 1\nset igmp_snooping.enabled true\nset stp.mode rstp\nset stp.priority 4096\nset ports.lan1.stp.edge true\nset vlans.10.name office\nset vlans.10.address 10.10.0.1/24\nset vlans.10.dhcp.enabled true\nset vlans.10.dhcp.range_start 10.10.0.100\nset vlans.10.dhcp.range_end 10.10.0.120\nset routing.enabled true\nset routing.static.branch.prefix 192.168.44.0/24\nset routing.static.branch.via 10.10.0.9\ncommit\nexit\nexit\n' |
  ssh "$HOST" 'sudo einheit_s5' >/dev/null 2>&1
assert_in "hostname committed" "power-soak" "$(box hostname)"
commits_before=$(box 'sudo grep -c "^COMMIT " /var/lib/einheit/s5/confd.state')
echo "  commits in history: ${commits_before}"

if ! virsh -c "$LIBVIRT" domstate "$VM" >/dev/null 2>&1; then
  echo "cannot reach libvirt domain '$VM' at $LIBVIRT — is this a VM target?"
  exit 1
fi

for cycle in $(seq 1 "$CYCLES"); do
  echo "== cycle ${cycle}/${CYCLES}: power cut"
  virsh -c "$LIBVIRT" destroy "$VM" >/dev/null 2>&1
  sleep 2
  virsh -c "$LIBVIRT" start "$VM" >/dev/null 2>&1
  if ! wait_for_ssh; then
    bad "cycle ${cycle}: target came back" "no ssh after 120s"
    continue
  fi
  unit_state=$(wait_for_boot_unit)
  case "$unit_state" in
    active)
      ok "cycle ${cycle}: boot unit ran and succeeded"
      ;;
    inactive)
      # Not a hang and not a failure: systemd never started the unit on
      # this boot. Keep going and let the assertions below say whether
      # the box came back configured anyway.
      bad "cycle ${cycle}: boot unit ran" "ActiveState=inactive (never started)"
      ;;
    *)
      bad "cycle ${cycle}: boot unit ran" "ActiveState=${unit_state}"
      ;;
  esac
  assert_in "cycle ${cycle}: hostname restored" "power-soak" \
    "$(box hostname)"
  assert_in "cycle ${cycle}: bridge rebuilt" "br0" \
    "$(box 'ip -br link show br0')"
  assert_in "cycle ${cycle}: vlan_filtering on" "1" \
    "$(box 'cat /sys/class/net/br0/bridge/vlan_filtering')"
  assert_in "cycle ${cycle}: lan1 enslaved" "br0" \
    "$(box 'readlink /sys/class/net/lan1/master')"
  assert_in "cycle ${cycle}: shut port stayed shut" "state DOWN" \
    "$(box 'ip link show lan3 | head -1')"
  assert_in "cycle ${cycle}: VLAN 77 restored" "77 PVID Egress Untagged" \
    "$(box 'sudo /usr/sbin/bridge vlan show' | tr -s ' ')"
  assert_in "cycle ${cycle}: port MTU restored" "9000" \
    "$(box 'cat /sys/class/net/lan1/mtu')"
  assert_in "cycle ${cycle}: MAC ageing restored" "60000" \
    "$(box 'cat /sys/class/net/br0/bridge/ageing_time')"
  assert_in "cycle ${cycle}: static MAC restored" "aa:bb:cc:dd:ee:01" \
    "$(box 'sudo /usr/sbin/bridge fdb show | grep aa:bb:cc:dd:ee:01')"
  assert_in "cycle ${cycle}: IGMP snooping restored" "1" \
    "$(box 'cat /sys/class/net/br0/bridge/multicast_snooping')"
  # Phase 2. Each of these is a service that had to be restarted from a
  # configuration file regenerated during this boot: /var/run is a
  # tmpfs, so "it was there before the cut" proves nothing.
  assert_in "cycle ${cycle}: spanning tree restored" "2" \
    "$(box 'cat /sys/class/net/br0/bridge/stp_state')"
  assert_in "cycle ${cycle}: bridge priority restored" "1.000" \
    "$(box 'sudo /usr/sbin/mstpctl showbridge br0 | grep "bridge id"')"
  assert_in "cycle ${cycle}: SVI address restored" "10.10.0.1/24" \
    "$(box 'ip -o -4 addr show dev br0.10')"
  assert_in "cycle ${cycle}: bridge VLAN membership restored" "10" \
    "$(box 'sudo /usr/sbin/bridge vlan show dev br0 self')"
  assert_in "cycle ${cycle}: forwarding restored" "1" \
    "$(box 'cat /proc/sys/net/ipv4/ip_forward')"
  assert_in "cycle ${cycle}: static route restored" "192.168.44.0/24" \
    "$(box 'ip -4 route show')"
  assert_in "cycle ${cycle}: DHCP server regenerated and running" \
    "einheit/dnsmasq.conf" "$(box 'pgrep -a dnsmasq')"
  assert_in "cycle ${cycle}: the pool came back" \
    "dhcp-range=set:vlan10,10.10.0.100,10.10.0.120" \
    "$(box 'sudo cat /var/run/einheit/dnsmasq.conf')"
  assert_in "cycle ${cycle}: boot report says it ran this boot" "yes" \
    "$(printf 'show system boot\nexit\n' | ssh "$HOST" 'sudo einheit_s5' \
       2>/dev/null | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep ran_this_boot)"
done

# Boot-restore re-applies an existing commit; it must not record a new
# one. Otherwise a box that power-cycles for a week accumulates a commit
# per boot and buries the operator's own history.
echo "== history did not grow"
commits_after=$(box 'sudo grep -c "^COMMIT " /var/lib/einheit/s5/confd.state')
if [ "$commits_after" = "$commits_before" ]; then
  ok "commit count unchanged over ${CYCLES} power cuts (${commits_after})"
else
  bad "commit count grew over ${CYCLES} power cuts" \
    "before=${commits_before} after=${commits_after}"
fi

echo
echo "passed ${PASS}, failed ${FAIL}"
[ "$FAIL" -eq 0 ]
