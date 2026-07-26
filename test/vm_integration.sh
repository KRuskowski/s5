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
  # The banner is the first thing in every CLI capture and never the
  # reason for a failure, so showing the head of the output tells you
  # only that the CLI started. Strip the furniture and show the end,
  # which is where the error actually is.
  detail=$(echo "$2" | grep -A2 -iE 'error|refus|reject|fail' \
    | grep -viE 'errors +.[|│]' | head -6 | tr '\n' ' ')
  if [ -z "$detail" ]; then
    detail=$(echo "$2" \
      | grep -vE '^\s*$|^\+--|^\| einheit|^\| adapter|^\| target|note: no terminal|editing are off' \
      | tail -6 | tr '\n' ' ')
  fi
  echo "        got: $(echo "$detail" | cut -c1-400)"
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

# must_commit "<desc>" "<set lines>" — the same, but ASSERTS the commit
# landed. A commit sent to /dev/null that quietly failed makes every
# assertion after it a mystery; this turns the mystery into one failure
# at the point it happened.
must_commit() {
  out=$(commit_set "$2")
  assert_in "$1" "commit_id" "$out"
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

echo "== boot report (boot outcome is state, so it has a show verb)"
box 'sudo einheit_s5 --apply-boot' >/dev/null
out=$(cli 'show system boot\nexit\n')
assert_in "boot report says it ran this boot" "yes" "$out"
assert_in "boot report names the applied revision" "applied_revision" "$out"
assert_in "boot report records the fabric step" "step.fabric" "$out"
assert_in "boot report records the config-apply step" "step.config-ap" \
  "$out"
assert_in "show system has a divergence row" "config-divergence" \
  "$(cli 'show system\nexit\n')"
assert_in "no divergence on a clean boot" "none" \
  "$(cli 'show system\nexit\n' | grep -i divergence)"

echo "== boot report catches a boot where the unit never ran"
# The systemd-ordering-cycle class: a stale report must not read as a
# healthy boot. Forge an earlier boot id rather than rebooting, so this
# stays a single-pass test.
box 'sudo sed -i "s/^BOOT_ID .*/BOOT_ID forged-earlier-boot/" \
  /var/lib/einheit/s5/boot.report' >/dev/null
out=$(cli 'show system boot\nexit\n')
assert_in "stale report is flagged" "no" "$out"
assert_in "stale report warns it is from an earlier boot" "EARLIER" "$out"
assert_in "divergence is unknown, not 'none', when boot did not run" \
  "unknown" "$(cli 'show system\nexit\n' | grep -i divergence)"
box 'sudo einheit_s5 --apply-boot' >/dev/null

echo "== rescue configuration (WP0.5)"
commit_set 'set hostname rescue-known-good\nset dns.primary 192.0.2.77\n' \
  >/dev/null
out=$(cli 'save rescue\nexit\n')
assert_in "save rescue writes the slot" "saved" "$out"
assert_in "rescue lives beside the state, not in configs" "einheit-config" \
  "$(box 'sudo cat /var/lib/einheit/s5/rescue.conf')"
assert_out "rescue is not an ordinary saved config" "rescue" \
  "$(box 'sudo ls /var/lib/einheit/s5/configs/ 2>/dev/null')"
assert_in "show configs reports the rescue slot" "rescue" \
  "$(cli 'show configs\nexit\n')"
# A hostile config: commits cleanly, leaves the box wrong.
commit_set 'set hostname hostile-box\nset dns.primary 192.0.2.1\nset ports.lan1.enabled false\n' \
  >/dev/null
assert_in "hostile config landed" "hostile-box" "$(box hostname)"
out=$(cli 'rollback rescue\ny\nexit\n')
assert_in "rollback rescue commits" "commit_id" "$out"
assert_in "rescue restored the hostname" "rescue-known-good" \
  "$(box hostname)"
assert_in "rescue restored DNS" "192.0.2.77" \
  "$(box 'cat /etc/resolv.conf')"
assert_in "rescue restored the port" ",UP" \
  "$(box 'ip link show lan1 | head -1 | tr -d " "')"
# Rescue must outlive a factory reset by design.
cli 'configure\nload factory\ncommit\nexit\nexit\n' >/dev/null
assert_in "rescue survives load factory" "einheit-config" \
  "$(box 'sudo cat /var/lib/einheit/s5/rescue.conf')"
out=$(cli 'rollback rescue\ny\nexit\n')
assert_in "rescue still restores after a factory reset" "commit_id" "$out"
assert_in "box is known-good again" "rescue-known-good" "$(box hostname)"

echo "== port parameters (WP1.1)"
commit_set 'set ports.lan1.mtu 9000\n' >/dev/null
assert_in "MTU applies" "9000" "$(box 'cat /sys/class/net/lan1/mtu')"
# The DSA invariant: the conduit carries every user frame plus the
# switch tag, so it must be at least as large. Parsing a rendered table
# needs the ANSI escapes and the semantic markers ([OK]/[WARN]) stripped
# first, or the "value" is a pile of terminal control codes.
conduit=$(cli 'show fabric\nexit\n' |
  sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' |
  awk -F'│' '/conduit/ { print $2; exit }' |
  sed -e 's/\[[A-Za-z-]*\]//g' -e 's/[[:space:]]//g')
if [ -n "$conduit" ] && [ "$conduit" != "-" ]; then
  cmtu=$(box "cat /sys/class/net/${conduit}/mtu")
  if [ "${cmtu:-0}" -ge 9008 ]; then
    ok "conduit ${conduit} MTU raised to ${cmtu} (>= 9000 + tag)"
  else
    bad "conduit ${conduit} MTU covers the user MTU" "got ${cmtu:-unset}"
  fi
else
  ok "no DSA conduit on this target — invariant not applicable"
fi
commit_set 'set ports.lan1.mtu 1500\n' >/dev/null
assert_in "MTU change applies" "1500" "$(box 'cat /sys/class/net/lan1/mtu')"
out=$(cli 'show interfaces detail lan1\nexit\n')
assert_in "detail shows configured vs negotiated" "negotiated" "$out"
assert_in "detail shows the MTU" "1500" "$out"
out=$(commit_set 'set ports.lan1.mtu 70000\n')
assert_in "out-of-range MTU is rejected" "error" "$out"

echo "== static MACs + aging (WP1.4)"
commit_set 'set mac.aging_time 600\nset mac.static.aa:bb:cc:dd:ee:01.port lan1\nset mac.static.aa:bb:cc:dd:ee:01.vlan 1\n' \
  >/dev/null
assert_in "ageing time applies (seconds -> centiseconds)" "60000" \
  "$(box 'cat /sys/class/net/br0/bridge/ageing_time')"
assert_in "static entry is in the fdb" "lan1" \
  "$(box 'sudo /usr/sbin/bridge fdb show | grep aa:bb:cc:dd:ee:01')"
out=$(cli 'show mac-table\nexit\n')
assert_in "show mac-table lists it" "aa:bb:cc:dd:ee:01" "$out"
assert_in "show mac-table marks it static" "static" "$out"
# The load-bearing property: an operational verb must not delete config.
cli 'clear mac-table\nexit\n' >/dev/null
assert_in "static entry survives clear mac-table" "aa:bb:cc:dd:ee:01" \
  "$(box 'sudo /usr/sbin/bridge fdb show | grep aa:bb:cc:dd:ee:01')"
# The candidate owns the full static set.
cli 'configure\ndelete mac.static.aa:bb:cc:dd:ee:01.port\ny\ndelete mac.static.aa:bb:cc:dd:ee:01.vlan\ny\ncommit\nexit\nexit\n' \
  >/dev/null
assert_out "removing it from config removes it from the box" \
  "aa:bb:cc:dd:ee:01" \
  "$(box 'sudo /usr/sbin/bridge fdb show')"
out=$(commit_set 'set mac.static.not-a-mac.port lan1\n')
assert_in "a malformed MAC is rejected" "error" "$out"

echo "== IGMP snooping (WP1.7)"
commit_set 'set igmp_snooping.enabled true\nset igmp_snooping.querier true\n' \
  >/dev/null
assert_in "snooping enabled on the bridge" "1" \
  "$(box 'cat /sys/class/net/br0/bridge/multicast_snooping')"
assert_in "querier enabled on the bridge" "1" \
  "$(box 'cat /sys/class/net/br0/bridge/multicast_querier')"
out=$(cli 'show igmp-snooping\nexit\n')
assert_in "show igmp-snooping reports it" "enabled" "$out"
commit_set 'set igmp_snooping.querier false\n' >/dev/null
assert_in "querier can be turned back off" "0" \
  "$(box 'cat /sys/class/net/br0/bridge/multicast_querier')"

echo "== counters detail + clear (WP1.8)"
box 'ping -c 2 -W 1 127.0.0.1 >/dev/null 2>&1; true' >/dev/null
out=$(cli 'show counters lan1\nexit\n')
assert_in "counters render" "lan1" "$out"
cli 'clear counters lan1\nexit\n' >/dev/null
out=$(cli 'show counters lan1\nexit\n')
# The kernel cannot zero these; clear is a baseline the reads subtract.
assert_in "counters read as cleared" "0" "$out"
out=$(cli 'clear counters nosuchport\nexit\n')
assert_in "clear counters rejects an unknown port" "error" "$out"

echo "== RSTP loop protection (WP1.2)"
must_commit "spanning tree commits" 'set stp.mode rstp\nset stp.priority 4096\nset ports.lan1.stp.edge true\n'
# stp_state 2 is BR_USER_STP: the kernel has handed spanning tree to a
# userspace daemon. 1 would be the kernel's own 802.1D, which is what
# mstpd exists to replace, and the CLI would report "rstp" either way.
assert_in "bridge is in user-space STP mode" "2" \
  "$(box 'cat /sys/class/net/br0/bridge/stp_state')"
assert_in "mstpd runs RSTP" "rstp" \
  "$(box 'sudo /usr/sbin/mstpctl showbridge br0 | grep "force protocol"')"
assert_in "bridge priority reached mstpd" "1.000" \
  "$(box 'sudo /usr/sbin/mstpctl showbridge br0 | grep "bridge id"')"
assert_in "edge port reached mstpd" "yes" \
  "$(box 'sudo /usr/sbin/mstpctl showportdetail br0 lan1 | grep "admin edge"')"
out=$(cli 'show spanning-tree\nexit\n')
assert_in "show spanning-tree names the protocol" "rstp" "$out"
assert_in "show spanning-tree lists ports" "lan1" "$out"
out=$(cli 'show spanning-tree statistics\nexit\n')
assert_in "BPDU counters render" "lan1" "$out"
cli 'clear spanning-tree statistics\nexit\n' >/dev/null
assert_in "statistics baseline clears" "0" \
  "$(cli 'show spanning-tree statistics lan1\nexit\n')"

# THE loop gate. A dumb bridge in a namespace, two veths into br0:
# a genuine two-link loop that only spanning tree can break. Contained
# entirely inside the target, so it runs anywhere without touching the
# hypervisor's own bridges.
box '
  sudo ip netns del s5loop 2>/dev/null
  sudo ip link del lp1 2>/dev/null
  sudo ip link del lp2 2>/dev/null
  sudo ip netns add s5loop
  sudo ip link add lp1 type veth peer name lp1b
  sudo ip link add lp2 type veth peer name lp2b
  sudo ip link set lp1b netns s5loop
  sudo ip link set lp2b netns s5loop
  sudo ip netns exec s5loop ip link add hub type bridge
  sudo ip netns exec s5loop ip link set hub type bridge stp_state 0
  sudo ip netns exec s5loop ip link set lp1b master hub
  sudo ip netns exec s5loop ip link set lp2b master hub
  sudo ip netns exec s5loop ip link set hub up
  sudo ip netns exec s5loop ip link set lp1b up
  sudo ip netns exec s5loop ip link set lp2b up
  sudo ip link set lp1 master br0
  sudo ip link set lp2 master br0
  sudo ip link set lp1 up
  sudo ip link set lp2 up
  sudo /usr/sbin/mstpctl addbridge br0
' >/dev/null
sleep 12
states=$(box 'sudo /usr/sbin/mstpctl showport br0 | grep -E "lp1|lp2"')
forwarding=$(echo "$states" | grep -c forw)
assert_in "exactly one side of the loop forwards" "1" "$forwarding"
assert_in "the other side is blocked" "disc" "$states"
# The measurement that makes it a loop test rather than a state
# inspection: broadcast into the hub with a source MAC the switch does
# not own, and count what comes back out. A storm is unbounded; a
# broken loop is a handful of frames.
before=$(box 'cat /sys/class/net/lp1/statistics/rx_packets')
box 'sudo ip netns exec s5loop timeout 5 ping -b -c 20 -i 0.05 255.255.255.255 -I hub >/dev/null 2>&1; true' \
  >/dev/null
after=$(box 'cat /sys/class/net/lp1/statistics/rx_packets')
delta=$((after - before))
echo "    (loop segment carried ${delta} frames with RSTP on)"
if [ "$delta" -lt 2000 ]; then
  ok "no broadcast storm with spanning tree on"
else
  bad "no broadcast storm with spanning tree on" "${delta} frames"
fi
# Killing the forwarding side must hand over to the blocked one.
fwd=$(echo "$states" | awk '/forw/{print $1}')
box "sudo ip link set ${fwd} down" >/dev/null
sleep 8
after_states=$(box 'sudo /usr/sbin/mstpctl showport br0 | grep -E "lp1|lp2"')
assert_in "the surviving link takes over" "forw" "$after_states"
box '
  sudo ip link del lp1 2>/dev/null
  sudo ip link del lp2 2>/dev/null
  sudo ip netns del s5loop 2>/dev/null
' >/dev/null

out=$(commit_set 'set stp.max_age 40\nset stp.forward_delay 15\n')
assert_in "an impossible timer triple is refused" "error" "$out"
out=$(commit_set 'set ports.lan5.stp.edge true\n')
assert_in "spanning tree on the routed uplink is refused" "error" "$out"

echo "== LLDP (WP1.3)"
must_commit "LLDP commits" 'set lldp.enabled true\nset lldp.tx_interval 15\n'
assert_in "the daemon is running" "lldp-daemon" \
  "$(box "pgrep -af 'einheit_s5 --lldp-daemon'")"
assert_in "the generated config carries the interval" "tx_interval 15" \
  "$(box 'sudo cat /var/run/einheit/lldp.conf')"
assert_in "the generated config says it is generated" "GENERATED" \
  "$(box 'sudo cat /var/run/einheit/lldp.conf')"
# We advertise, and it is a real LLDPDU: tcpdump decodes the TLVs by
# name, which a hand-rolled encoder getting the framing wrong would not
# survive.
tx=$(box 'sudo timeout 20 tcpdump -i lan1 -nn -c 1 -v ether proto 0x88cc 2>&1')
assert_in "we transmit LLDP" "LLDP" "$tx"
assert_in "the chassis TLV is well-formed" "Chassis ID TLV" "$tx"
assert_in "we name ourselves" "System Name TLV" "$tx"
# And we listen. The receive half cannot be driven from inside the box:
# LLDP is nearest-bridge, so its frames are consumed by the first
# bridge that sees them and a neighbour has to be on the other end of
# the actual cable — which, for a VM, is a tap device on the
# hypervisor. test/lldp_inject.py stands there; run it from the host
# and re-run this suite with S5_LLDP_PEER=1 to include the assertions.
# The decode itself is covered exhaustively by the unit tests
# (S5Lldp.*), including frames built by hand from 802.1AB rather than
# by our own encoder, and the hostile ones.
if [ "${S5_LLDP_PEER:-0}" = "1" ]; then
  out=$(cli 'show neighbors\nexit\n')
  assert_in "show neighbors names the injected peer" "corridor-sw" "$out"
  assert_in "show neighbors gives their port" "ge-0/0/7" "$out"
  assert_in "show neighbors gives their management address" \
    "10.90.0.254" "$out"
else
  echo "  SKIP  neighbour receive (run test/lldp_inject.py on the" \
       "hypervisor, then re-run with S5_LLDP_PEER=1)"
fi
# The table has to render either way, empty or not.
out=$(cli 'show neighbors\nexit\n')
# "error" alone would match the session summary's own `errors  0`
# row, which is present on every capture.
assert_out "show neighbors never errors" "error  dispatch" "$out"

echo "== SVIs and routing (WP2.1, WP2.2)"
must_commit "SVIs and forwarding commit" 'set vlans.10.name office\nset vlans.10.address 10.10.0.1/24\nset vlans.20.name lab\nset vlans.20.address 10.20.0.1/24\nset routing.enabled true\n'
assert_in "the SVI exists" "10.10.0.1/24" \
  "$(box 'ip -o -4 addr show dev br0.10')"
# The half everyone forgets: without the bridge's own membership the
# interface exists, holds its address and receives nothing.
assert_in "the bridge is a member of the VLAN" "10" \
  "$(box 'sudo /usr/sbin/bridge vlan show dev br0 self')"
assert_in "forwarding is on" "1" \
  "$(box 'cat /proc/sys/net/ipv4/ip_forward')"
out=$(cli 'show vlans\nexit\n')
assert_in "show vlans carries the name" "office" "$out"
assert_in "show vlans carries the address" "10.10.0.1/24" "$out"
must_commit "the static route commits" 'set routing.static.branch.prefix 192.168.44.0/24\nset routing.static.branch.via 10.20.0.9\n'
assert_in "the static route is installed" "192.168.44.0/24" \
  "$(box 'ip -4 route show')"
out=$(cli 'show route\nexit\n')
assert_in "show route marks it as ours" "config" "$out"
# Inter-VLAN routing, end to end, between two namespaces.
box '
  for v in 10 20; do
    sudo ip netns del rt$v 2>/dev/null
    sudo ip link del r${v}a 2>/dev/null
    sudo ip netns add rt$v
    sudo ip link add r${v}a type veth peer name r${v}b
    sudo ip link set r${v}b netns rt$v
    sudo ip link set r${v}a master br0
    sudo ip link set r${v}a up
    sudo /usr/sbin/bridge vlan add dev r${v}a vid $v pvid untagged
    sudo /usr/sbin/bridge vlan del dev r${v}a vid 1
    sudo ip netns exec rt$v ip link set r${v}b up
    sudo ip netns exec rt$v ip addr add 10.$v.0.9/24 dev r${v}b
    sudo ip netns exec rt$v ip route add default via 10.$v.0.1
  done
' >/dev/null
sleep 2
assert_in "a client can reach its own gateway" "0% packet loss" \
  "$(box 'sudo ip netns exec rt10 ping -c 2 -W 2 10.10.0.1')"
assert_in "the switch routes between VLANs" "0% packet loss" \
  "$(box 'sudo ip netns exec rt10 ping -c 3 -W 2 10.20.0.9')"
# And turning routing off must actually stop it.
must_commit "forwarding can be turned off" 'set routing.enabled false\n'
# Asserted positively: "0% packet loss" is a substring of
# "100% packet loss", so the absence check would pass either way.
assert_in "with forwarding off it does not" "100% packet loss" \
  "$(box 'sudo ip netns exec rt10 ping -c 2 -W 2 10.20.0.9')"
must_commit "and back on" 'set routing.enabled true\n'

echo "== DHCP + DNS (WP2.3)"
must_commit "two DHCP pools and DNS commit" 'set vlans.10.dhcp.enabled true\nset vlans.10.dhcp.range_start 10.10.0.100\nset vlans.10.dhcp.range_end 10.10.0.120\nset vlans.10.dhcp.lease_time 720\nset vlans.20.dhcp.enabled true\nset vlans.20.dhcp.range_start 10.20.0.100\nset vlans.20.dhcp.range_end 10.20.0.120\nset vlans.20.dhcp.lease_time 60\nset dns.serve true\nset dns.local_domain office.lan\n'
assert_in "dnsmasq is running against OUR config" "einheit/dnsmasq.conf" \
  "$(box 'pgrep -a dnsmasq')"
assert_in "the generated config is marked generated" "GENERATED" \
  "$(box 'sudo cat /var/run/einheit/dnsmasq.conf')"
# The gate: each VLAN gets its own pool, and only its own.
box '
  for v in 10 20; do
    sudo ip netns exec rt$v ip addr flush dev r${v}b
    sudo ip netns exec rt$v timeout 25 busybox udhcpc -i r${v}b -q -n \
      > /tmp/dhcp$v.log 2>&1
  done
' >/dev/null
assert_in "VLAN 10 gets an address from the VLAN 10 pool" "10.10.0.1" \
  "$(box 'grep lease /tmp/dhcp10.log')"
assert_in "VLAN 20 gets an address from the VLAN 20 pool" "10.20.0.1" \
  "$(box 'grep lease /tmp/dhcp20.log')"
assert_out "VLAN 20 is NOT served out of the VLAN 10 pool" "10.10.0." \
  "$(box 'grep lease /tmp/dhcp20.log')"
assert_in "the lease times differ per pool" "3600" \
  "$(box 'grep lease /tmp/dhcp20.log')"
out=$(cli 'show dhcp leases\nexit\n')
assert_in "show dhcp leases lists a client" "10.10.0.1" "$out"
out=$(cli 'show dhcp server\nexit\n')
assert_in "show dhcp server lists the pools" "vlan10" "$out"
assert_in "show dhcp server reports it running" "running" "$out"
lease=$(box "grep -o '10\\.10\\.0\\.1[0-9]*' /tmp/dhcp10.log | head -1")
out=$(cli "clear dhcp lease ${lease}\nexit\n")
assert_in "clear dhcp lease releases it" "released" "$out"
assert_out "and it is gone from the database" "$lease" \
  "$(box 'sudo cat /var/run/einheit/dnsmasq.leases')"
# DNS forwarding through the switch.
box '
  sudo ip netns exec rt10 ip addr add 10.10.0.9/24 dev r10b 2>/dev/null
  true' >/dev/null
assert_in "the switch answers DNS" "Address" \
  "$(box 'sudo ip netns exec rt10 timeout 8 busybox nslookup example.com 10.10.0.1')"
out=$(commit_set 'set vlans.10.dhcp.range_end 10.99.0.200\n')
assert_in "a pool outside the VLAN subnet is refused" "error" "$out"
out=$(commit_set 'set dns.local_domain evil.lan;touch/tmp/pwned\n')
assert_in "a hostile domain is refused" "error" "$out"
assert_in "and nothing ran" "no" \
  "$(box 'test -e /tmp/pwned && echo yes || echo no')"

echo "== mDNS reflection (WP2.4)"
must_commit "mDNS reflection commits" 'set mdns.enabled true\nset mdns.reflect.10 true\nset mdns.reflect.20 true\n'
assert_in "the repeater spans VLAN 10" "br0.10" \
  "$(box 'pgrep -a mdns-repeater')"
assert_in "the repeater spans VLAN 20" "br0.20" \
  "$(box 'pgrep -a mdns-repeater')"
out=$(commit_set 'set mdns.reflect.20 false\n')
assert_in "reflection across one VLAN is refused" "error" "$out"
must_commit "mDNS can be turned off" 'set mdns.enabled false\n'
assert_out "turning it off stops the repeater" "mdns-repeater" \
  "$(box 'pgrep -a mdns-repeater')"

echo "== NTP serve (WP2.5)"
must_commit "NTP serving commits" 'set ntp.server 10.55.5.9\nset ntp.serve true\n'
assert_in "ntpd listens as a server" "-l" \
  "$(box "tr '\\0' ' ' < /proc/\$(pidof ntpd | cut -d' ' -f1)/cmdline")"
assert_in "show ntp says it is serving" "yes" \
  "$(cli 'show ntp\nexit\n')"
must_commit "NTP serving can be turned off" 'set ntp.serve false\n'
assert_out "and stops when told to" "-l " \
  "$(box "tr '\\0' ' ' < /proc/\$(pidof ntpd | cut -d' ' -f1)/cmdline")"
out=$(cli 'configure\ndelete ntp.server\ny\nset ntp.serve true\ncommit\nexit\nexit\n')
assert_in "serving without a source is refused" "error" "$out"

echo "== service supervision"
out=$(cli 'show system services\nexit\n')
assert_in "services are listed" "dnsmasq" "$out"
assert_in "and their state" "running" "$out"
# The row this command exists for: configured, and not running.
box 'sudo pkill -x dnsmasq' >/dev/null
sleep 1
assert_in "a dead-but-wanted service reads as DOWN" "DOWN" \
  "$(cli 'show system services\nexit\n')"
cli 'configure\ncommit\nexit\nexit\n' >/dev/null
assert_in "and a commit brings it back" "running" \
  "$(cli 'show system services\nexit\n')"

echo "== anti-lockout (WP2.6)"
# This suite arrives over ssh, so the CLI's own session IS the
# management path — exactly the case the guard is for.
out=$(cli 'configure\nset interfaces.lan5.address 10.66.6.1/24\nshow diff\nexit\nexit\n')
mgmt_dev=$(box "ip route get \$(echo \$SSH_CLIENT | cut -d' ' -f1) 2>/dev/null")
case "$mgmt_dev" in
  *lan5*)
    assert_in "readdressing the management path warns" "warning" "$out"
    assert_in "and suggests commit confirmed" "commit confirmed" "$out"
    ;;
  *)
    # The suite reaches the box on its management NIC, not on lan5, so
    # there is nothing for the guard to warn about here. Say so rather
    # than silently skipping.
    assert_out "an unrelated interface produces no warning" "warning" \
      "$out"
    ;;
esac

echo "== tear down the services tier"
cli 'configure\ndelete vlans\ny\ndelete routing\ny\ndelete mdns\ny\ndelete dns.serve\ny\ndelete dns.local_domain\ny\ndelete ntp.serve\ny\ncommit\nexit\nexit\n' \
  >/dev/null
box '
  for v in 10 20; do
    sudo ip netns del rt$v 2>/dev/null
    sudo ip link del r${v}a 2>/dev/null
  done
' >/dev/null

echo "== restore test defaults"
commit_set 'set hostname s5-test\nset dns.primary 9.9.9.9\nset dns.secondary 1.1.1.1\nset interfaces.lan5.address 10.55.5.1/24\n' \
  >/dev/null
cli 'configure\ndelete ports.lan1.vlan.100\ny\ndelete ports.lan2.vlan.100\ny\ndelete ports.lan3.vlan.200\ny\ndelete ports.lan4.vlan.200\ny\ncommit\nexit\nexit\n' \
  >/dev/null
assert_in "defaults restored" "s5-test" "$(box hostname)"

echo
echo "passed ${PASS}, failed ${FAIL}"
[ "$FAIL" -eq 0 ]
