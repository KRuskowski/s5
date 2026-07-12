#!/usr/bin/env bash
# Exhaustive config-surface integration test for the s5 CLI.
#
# Sets and changes EVERY schema path through the real
# configure/set/commit lifecycle and verifies each apply on the box
# itself (hostname/resolv.conf/ip/bridge), then exercises the
# negative paths (validation rejects, fail-before-write, strict
# arity). Requires a DISPOSABLE target prepared like the s5-test
# VM: einheit_s5 installed, lan1..lan4 enslaved to a
# vlan_filtering bridge br0, lan5 left routed, passwordless sudo,
# and ntpd_sim.c installed as /usr/local/sbin/ntpd (Debian has no
# busybox ntpd applet; see that file's header).
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

echo "== restore test defaults"
commit_set 'set hostname s5-test\nset dns.primary 9.9.9.9\nset dns.secondary 1.1.1.1\nset interfaces.lan5.address 10.55.5.1/24\n' \
  >/dev/null
cli 'configure\ndelete ports.lan1.vlan.100\ny\ndelete ports.lan2.vlan.100\ny\ndelete ports.lan3.vlan.200\ny\ndelete ports.lan4.vlan.200\ny\ncommit\nexit\nexit\n' \
  >/dev/null
assert_in "defaults restored" "s5-test" "$(box hostname)"

echo
echo "passed ${PASS}, failed ${FAIL}"
[ "$FAIL" -eq 0 ]
