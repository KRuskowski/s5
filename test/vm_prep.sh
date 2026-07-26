#!/usr/bin/env bash
# Idempotent test-VM preparation: everything the s5-test VM needs
# beyond a bare deb-01 clone.
#
# It used to hand-build the bridge. It no longer does — the product owns
# its switch fabric now (ROADMAP Phase 0), so this TEARS DOWN any
# leftover bridge instead. The suite then proves einheit_s5 constructs
# br0 + vlan_filtering + enslavement from nothing rather than silently
# inheriting a fabric a prep script made for it. That is the
# fabric-bootstrap lesson: no ssh setup for something the product
# should own.
#
# Usage: vm_prep.sh [ssh-host]
set -eu
HOST=${1:-s5-test}
ssh "$HOST" '
  set -e
  # Leave the switch ports bare: no master, and no bridge at all.
  for p in lan1 lan2 lan3 lan4 lan5; do
    sudo ip link set "$p" nomaster 2>/dev/null || true
  done
  sudo ip link del br0 2>/dev/null || true
  # Anything a previous run enslaved or created.
  for i in lan9 lp1 lp2 r10a r20a c10a c20a; do
    sudo ip link del "$i" 2>/dev/null || true
  done
  for n in s5loop lldpns rt10 rt20 cli10 cli20; do
    sudo ip netns del "$n" 2>/dev/null || true
  done
  # And the DURABLE state. "Disposable target" has to mean the commit
  # history too: a suite that starts from the last run own commits is
  # testing whatever that run happened to leave behind, and an aborted
  # run leaves a box no later run can interpret.
  sudo pkill -x dnsmasq 2>/dev/null || true
  sudo pkill -x mdns-repeater 2>/dev/null || true
  sudo pkill -f "[e]inheit_s5 --lldp-daemon" 2>/dev/null || true
  sudo rm -rf /var/lib/einheit/s5 /var/run/einheit
  sudo ip link set lo up 2>/dev/null || true
  test -x /usr/local/sbin/ntpd || echo "WARN: ntpd sim missing (test/ntpd_sim.c)"
  test -x /usr/local/bin/einheit_s5 || echo "WARN: einheit_s5 not installed"
  echo prepared
'
