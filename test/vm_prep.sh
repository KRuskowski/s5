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
  test -x /usr/local/sbin/ntpd || echo "WARN: ntpd sim missing (test/ntpd_sim.c)"
  test -x /usr/local/bin/einheit_s5 || echo "WARN: einheit_s5 not installed"
  echo prepared
'
