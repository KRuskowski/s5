#!/usr/bin/env bash
# Exhaustive config-surface integration test for the s5 CLI.
#
# Sets and changes EVERY schema path through the real
# configure/set/commit lifecycle and verifies each apply on the box
# itself (hostname/resolv.conf/ip/bridge), then exercises the
# negative paths (validation rejects, fail-before-write, strict
# arity), then the Phase 0 structural surfaces: fabric bootstrap,
# boot-restore, edit locking and save/load/factory.
#
# Requires a DISPOSABLE target prepared with test/vm_prep.sh:
# einheit_s5 installed, lan1..lan5 present with NO bridge (the
# product builds its own fabric), passwordless sudo, and ntpd_sim.c
# installed as /usr/local/sbin/ntpd (Debian has no busybox ntpd
# applet; see that file's header).
# NEVER run against a production switch — it rewrites hostname,
# DNS and interface state (and restores test defaults at the end).
#
# Usage: test/vm_integration.sh [ssh-host]   # default: s5-test

set -u
HOST=${1:-s5-test}
PASS=0
FAIL=0

cli() {
  # shellcheck disable=SC2029
  printf '%b' "$1" | ssh "$HOST" 'sudo einheit_s5' 2>/dev/null
}

box() {
  # shellcheck disable=SC2029
  ssh "$HOST" "$1" 2>/dev/null
}

ok() {
  PASS=$((PASS + 1))
  echo "  PASS  $1"
}

bad() {
  FAIL=$((FAIL + 1))
  echo "  FAIL  $1"
  echo "        got: $(echo "$2" | head -2 | tr '\n' ' ')"
}

# assert_in <desc> <needle> <haystack>
assert_in() {
  case "$3" in
    *"$2"*) ok "$1" ;;
    *) bad "$1" "$3" ;;
  esac
}

# assert_out <desc> <needle> <haystack>
assert_out() {
  case "$3" in
    *"$2"*) bad "$1" "$3" ;;
    *) ok "$1" ;;
  esac
}

commit_set() {
  # commit_set "<set lines>" — one candidate, committed.
  cli "configure\n${1}commit\nexit\nexit\n"
}

echo "== fabric bootstrap (the product builds its own switch)"
# vm_prep.sh left the box with no bridge at all. Merely starting the
# CLI must construct the fabric: that is the difference between a
# product that owns its switch and one that needs a prep script.
assert_out "prep left no bridge" "br0" "$(box 'ip -br link show br0')"
cli 'show fabric\nexit\n' >/dev/null
assert_in "CLI start creates br0" "br0" "$(box 'ip -br link show br0')"
assert_in "vlan_filtering is on" "1" \
  "$(box 'cat /sys/class/net/br0/bridge/vlan_filtering')"
for p in lan1 lan2 lan3 lan4; do
  assert_in "${p} enslaved to br0" "br0" \
    "$(box "readlink /sys/class/net/${p}/master")"
done
assert_out "lan5 stays routed (it is the uplink)" "br0" \
  "$(box 'readlink /sys/class/net/lan5/master')"
out=$(cli 'show fabric\nexit\n')
assert_in "show fabric reports filtering" "yes" "$out"
assert_in "show fabric lists members" "lan1" "$out"

echo "== hostname"
commit_set 'set hostname it-alpha\n' >/dev/null
assert_in "set hostname applies" "it-alpha" "$(box hostname)"
commit_set 'set hostname it-beta\n' >/dev/null
assert_in "change hostname applies" "it-beta" "$(box hostname)"
cli 'rollback previous\ny\nexit\n' >/dev/null
assert_in "rollback restores hostname" "it-alpha" "$(box hostname)"

echo "== dns"
commit_set 'set dns.primary 9.9.9.10\nset dns.secondary 1.0.0.1\n' \
  >/dev/null
resolv=$(box 'cat /etc/resolv.conf')
assert_in "dns.primary applies" "9.9.9.10" "$resolv"
assert_in "dns.secondary applies" "1.0.0.1" "$resolv"
commit_set 'set dns.primary 8.8.4.4\n' >/dev/null
assert_in "change dns.primary applies" "8.8.4.4" \
  "$(box 'cat /etc/resolv.conf')"

echo "== ntp"
out=$(commit_set 'set ntp.server time.cloudflare.com\n')
assert_in "ntp.server commits" "commit_id" "$out"
assert_in "ntp.server in running config" "time.cloudflare.com" \
  "$(cli 'show config ntp\nexit\n')"

echo "== interface addressing (routed port + bridge SVI)"
commit_set 'set interfaces.lan5.address 10.55.7.1/24\n' >/dev/null
assert_in "lan5 static address applies" "10.55.7.1/24" \
  "$(box 'ip -br addr show lan5')"
commit_set 'set interfaces.lan5.address 10.55.8.1/24\n' >/dev/null
assert_in "lan5 address change applies" "10.55.8.1/24" \
  "$(box 'ip -br addr show lan5')"
commit_set 'set interfaces.br0.address 10.99.1.1/24\n' >/dev/null
assert_in "br0 address applies" "10.99.1.1/24" \
  "$(box 'ip -br addr show br0')"
commit_set 'set interfaces.lan5.dhcp true\n' >/dev/null
assert_out "dhcp flushes the static address" "10.55.8.1" \
  "$(box 'ip -br addr show lan5')"
cli 'rollback previous\ny\nexit\n' >/dev/null
assert_in "rollback restores lan5 address" "10.55.8.1/24" \
  "$(box 'ip -br addr show lan5')"

echo "== port admin state (every port)"
for p in lan1 lan2 lan3 lan4 lan5; do
  commit_set "set ports.${p}.enabled false\n" >/dev/null
  assert_out "disable ${p}" ",UP" \
    "$(box "ip link show ${p} | head -1 | tr -d ' '")"
  commit_set "set ports.${p}.enabled true\n" >/dev/null
  assert_in "enable ${p}" ",UP" \
    "$(box "ip link show ${p} | head -1 | tr -d ' '")"
done

echo "== VLANs (every mode, every bridged port)"
commit_set 'set ports.lan1.vlan.100 untagged-pvid\nset ports.lan2.vlan.100 tagged\nset ports.lan3.vlan.200 pvid\nset ports.lan4.vlan.200 untagged\nset ports.lan4.vlan.300 tagged\n' \
  >/dev/null
vlans=$(box 'sudo /usr/sbin/bridge vlan show')
assert_in "lan1 vid 100 untagged-pvid" \
  "100 PVID Egress Untagged" \
  "$(echo "$vlans" | grep -A2 '^lan1' | tr -s ' ')"
assert_in "lan2 vid 100 tagged" "100" \
  "$(echo "$vlans" | grep -A2 '^lan2' | tr -s ' ')"
assert_in "lan3 vid 200 pvid" "200 PVID" \
  "$(echo "$vlans" | grep -A2 '^lan3' | tr -s ' ')"
assert_in "lan4 vid 200 untagged" "200 Egress Untagged" \
  "$(echo "$vlans" | grep -A3 '^lan4' | tr -s ' ')"
assert_in "lan4 vid 300 tagged" "300" \
  "$(echo "$vlans" | grep -A3 '^lan4')"
# Flag change on an existing VID.
commit_set 'set ports.lan2.vlan.100 untagged-pvid\n' >/dev/null
assert_in "lan2 vid 100 flag change" "100 PVID Egress Untagged" \
  "$(box 'sudo /usr/sbin/bridge vlan show' | grep -A2 '^lan2' | tr -s ' ')"
# Delete a VID out of the candidate.
cli 'configure\ndelete ports.lan4.vlan.300\ny\ncommit\nexit\nexit\n' \
  >/dev/null
assert_out "delete lan4 vid 300" "300" \
  "$(box 'sudo /usr/sbin/bridge vlan show' | grep -A3 '^lan4')"

echo "== show diff before commit"
out=$(cli 'configure\nset hostname diff-probe\nshow diff\nexit\nexit\n')
assert_in "show diff lists pending change" \
  "hostname: diff-probe (was it-alpha)" "$out"
assert_in "uncommitted change is not applied" "it-alpha" \
  "$(box hostname)"

echo "== negative: validation and fail-before-write"
out=$(cli 'configure\nset ports.lan1.vlan.100 bogus\nexit\nexit\n')
assert_in "bad vlan mode rejected at set" "error" "$out"
out=$(cli 'configure\nset dns.primary not.an.ip\nexit\nexit\n')
assert_in "bad ip rejected at set" "error" "$out"
out=$(commit_set 'set hostname never-applied\nset ports.lan1.vlan.5000 tagged\n')
assert_in "vid 5000 rejected at commit" "VID must be 1-4094" "$out"
assert_in "rejected candidate applies nothing" "it-alpha" \
  "$(box hostname)"
out=$(commit_set 'set poe.1.enabled true\n')
assert_in "poe on PoE-less box rejected" "PoE bus unavailable" "$out"
out=$(commit_set 'set ports.lan5.vlan.100 tagged\n')
assert_in "vlan on unbridged port fails cleanly" "apply_failed" "$out"

echo "== negative: strict arity"
out=$(cli 'commit 5\nexit\n')
assert_in "commit rejects stray argument" "unexpected argument" "$out"
out=$(cli 'configure\nset interfaces.\nexit\nexit\n')
assert_in "set without value is an error" "value" "$out"

echo "== boot-restore (a switch that survives a reboot)"
commit_set 'set hostname boot-probe\nset ports.lan2.enabled false\n' \
  >/dev/null
assert_in "boot-probe committed" "boot-probe" "$(box hostname)"
# Drift the box behind confd's back — a reboot looks exactly like this
# from the management plane's point of view.
box 'sudo hostname wrong-after-reboot' >/dev/null
box 'sudo ip link set lan2 up' >/dev/null
assert_in "box drifted" "wrong-after-reboot" "$(box hostname)"
out=$(box 'sudo einheit_s5 --apply-boot')
assert_in "boot apply reports the restored commit" "restored commit" "$out"
assert_in "boot apply restores hostname" "boot-probe" "$(box hostname)"
assert_out "boot apply restores port admin state" ",UP" \
  "$(box 'ip link show lan2 | head -1 | tr -d " "')"
# Intent must beat the box's power-on state, not the other way round.
assert_in "running config still says the port is down" "false" \
  "$(cli 'show config ports.lan2\nexit\n')"

echo "== boot-restore rebuilds a torn-down fabric"
box 'sudo ip link del br0' >/dev/null
assert_out "bridge is gone" "br0" "$(box 'ip -br link show br0')"
out=$(box 'sudo einheit_s5 --apply-boot')
assert_in "boot apply reports the fabric" "fabric br0 up" "$out"
assert_in "bridge rebuilt" "br0" "$(box 'ip -br link show br0')"
assert_in "filtering rebuilt" "1" \
  "$(box 'cat /sys/class/net/br0/bridge/vlan_filtering')"
assert_in "lan1 re-enslaved" "br0" \
  "$(box 'readlink /sys/class/net/lan1/master')"
# Second run must be a no-op, not a rebuild — the exit gate is 50
# power pulls, so idempotence is the property that matters.
out=$(box 'sudo einheit_s5 --apply-boot')
assert_in "repeat boot apply still succeeds" "restored commit" "$out"
commit_set 'set ports.lan2.enabled true\n' >/dev/null

echo "== boot-restore is idempotent over repeated boots"
before=$(cli 'show config\nexit\n' | grep -c . || true)
for _ in 1 2 3 4 5; do
  box 'sudo einheit_s5 --apply-boot' >/dev/null
done
after=$(cli 'show config\nexit\n' | grep -c . || true)
assert_in "config unchanged after 5 boots" "$before" "$after"

echo "== edit locking"
# A second editor must be refused, not allowed to clobber the
# candidate. On s5 the two editors are two PROCESSES (confd is
# embedded in the CLI), so this needs a real concurrent session.
(printf 'configure\n'; sleep 12) | ssh "$HOST" 'sudo einheit_s5' \
  >/dev/null 2>&1 &
holder=$!
sleep 3
out=$(cli 'configure\nexit\nexit\n')
assert_in "second configure is refused" "held by" "$out"
# The hint gets its own rendered line; the message line is truncated at
# the box width, which is why the way out must not live in it.
assert_in "refusal points at the escape hatch" "configure force" "$out"
status=$(cli 'show status\nexit\n')
assert_in "show status names the lock holder" "lock_holder" "$status"
assert_in "show status names the lock session" "lock_session" "$status"
out=$(cli 'configure force\nset hostname lock-thief\ncommit\nexit\nexit\n')
assert_in "configure force takes over and commits" "commit_id" "$out"
assert_in "the thief's commit landed" "lock-thief" "$(box hostname)"
wait "$holder" 2>/dev/null || true
# The lock dies with the session: once both processes are gone,
# configure mode is free again without needing force.
out=$(cli 'configure\nexit\nexit\n')
assert_out "lock is free after the holders exit" "held by" "$out"

echo "== save / load / factory"
commit_set 'set hostname save-probe\nset dns.primary 192.0.2.99\n' \
  >/dev/null
out=$(cli 'save probe\nexit\n')
assert_in "save writes a named config" "saved" "$out"
assert_in "show configs lists it" "probe" "$(cli 'show configs\nexit\n')"
assert_in "saved file is on the box" "save-probe" \
  "$(box 'sudo cat /var/lib/einheit/s5/configs/probe.conf')"
commit_set 'set hostname drifted-away\nset dns.primary 192.0.2.1\n' \
  >/dev/null
assert_in "box drifted from the saved config" "drifted-away" \
  "$(box hostname)"
out=$(cli 'configure\nload replace probe\ncommit\nexit\nexit\n')
assert_in "load replace commits" "commit_id" "$out"
assert_in "load replace restored hostname" "save-probe" "$(box hostname)"
assert_in "load replace restored DNS" "192.0.2.99" \
  "$(box 'cat /etc/resolv.conf')"
# Loading stages a candidate; it must not touch the box on its own.
commit_set 'set hostname staged-only\n' >/dev/null
out=$(cli 'configure\nload replace probe\nshow diff\nexit\nexit\n')
assert_in "load shows up in the diff" "save-probe" "$out"
assert_in "load without commit changes nothing" "staged-only" \
  "$(box hostname)"
out=$(cli 'configure\nload merge probe\nexit\nexit\n')
assert_in "load merge is accepted" "loaded" "$out"
# The CLI runs as root: a config NAME must never be usable as a path.
out=$(cli 'configure\nload replace ../../etc/shadow\nexit\nexit\n')
assert_in "path traversal is rejected" "invalid config name" "$out"
out=$(cli 'save ../../tmp/pwned\nexit\n')
assert_in "save rejects traversal too" "invalid config name" "$out"
assert_out "nothing was written outside the config dir" "einheit-config" \
  "$(box 'cat /tmp/pwned.conf 2>/dev/null')"
out=$(cli 'configure\nload replace no-such-config\nexit\nexit\n')
assert_in "missing config is a clean error" "no such file" "$out"
out=$(cli 'configure\nload factory\nshow diff\nexit\nexit\n')
assert_in "load factory stages the shipped defaults" "einheit-s5" "$out"

echo "== first boot seeds the factory configuration"
# A factory-fresh box must not come up as a Linux box with every switch
# port administratively down. With no commit history, boot-restore
# applies the shipped defaults and records them as commit 1.
box 'sudo rm -f /var/lib/einheit/s5/confd.state' >/dev/null
out=$(box 'sudo einheit_s5 --apply-boot')
assert_in "virgin box seeds factory defaults" "seeded factory defaults" \
  "$out"
assert_in "factory hostname applied" "einheit-s5" "$(box hostname)"
assert_in "factory config is commit 1" "commit_id" \
  "$(cli 'show commits\nexit\n')"
# And the seed happens once: the next boot restores the operator's
# configuration, it does not reset to defaults again.
commit_set 'set hostname after-seed\n' >/dev/null
out=$(box 'sudo einheit_s5 --apply-boot')
assert_in "second boot restores, does not reseed" "restored commit" "$out"
assert_out "no reseed on the second boot" "seeded factory" "$out"
assert_in "operator config survived" "after-seed" "$(box hostname)"

echo "== restore test defaults"
commit_set 'set hostname s5-test\nset dns.primary 9.9.9.9\nset dns.secondary 1.1.1.1\nset interfaces.lan5.address 10.55.5.1/24\n' \
  >/dev/null
cli 'configure\ndelete ports.lan1.vlan.100\ny\ndelete ports.lan2.vlan.100\ny\ndelete ports.lan3.vlan.200\ny\ndelete ports.lan4.vlan.200\ny\ncommit\nexit\nexit\n' \
  >/dev/null
assert_in "defaults restored" "s5-test" "$(box hostname)"

echo
echo "passed ${PASS}, failed ${FAIL}"
[ "$FAIL" -eq 0 ]
