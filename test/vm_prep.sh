#!/usr/bin/env bash
# Idempotent test-VM preparation: everything the s5-test VM needs
# beyond a bare deb-01 clone. Run over ssh after boot (the bridge
# does not survive a reboot — the product's missing fabric
# bootstrap, ROADMAP Phase 0, will eventually own this on-box).
# Usage: vm_prep.sh [ssh-host]
set -eu
HOST=${1:-s5-test}
ssh "$HOST" '
  set -e
  sudo ip link add br0 type bridge 2>/dev/null || true
  sudo ip link set br0 type bridge vlan_filtering 1
  for p in lan1 lan2 lan3 lan4; do
    sudo ip link set "$p" master br0 2>/dev/null || true
    sudo ip link set "$p" up
  done
  sudo ip link set lan5 up
  sudo ip link set br0 up
  test -x /usr/local/sbin/ntpd || echo "WARN: ntpd sim missing (test/ntpd_sim.c)"
  test -x /usr/local/bin/einheit_s5 || echo "WARN: einheit_s5 not installed"
  echo prepared
'
