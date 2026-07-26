# On-device deployment

What has to be installed on an s5 box beyond the binary itself, and why.
Dev-board specifics for the T153 Lume live in `t153-dev-setup.md`.

## Files

| Source | Destination | Mode | Purpose |
|---|---|---|---|
| `build/einheit_s5` | `/usr/local/bin/einheit_s5` | 755 | CLI + management plane |
| `build/einheit-launch` | `/usr/local/bin/cli` | 4755 root:root | setuid launcher (the privilege boundary) |
| `deploy/einheit-s5-boot.service` | `/etc/systemd/system/` | 644 | boot-restore (systemd images) |
| `deploy/S39einheit-s5` | `/etc/init.d/S39einheit-s5` | 755 | boot-restore (BusyBox init — the production image) |
| `deploy/factory.conf.example` | `/etc/einheit/s5/factory.conf` | 644 | shipped defaults for `load factory` |

Durable state (running config, commit history, audit log, saved
configurations, the configure-mode lock) lives in `/var/lib/einheit/s5`,
created on first run. It must be on a writable, persistent filesystem —
on the production SPI-NAND image that means a writable overlay, not
squashfs.

## Boot-restore

Nothing else re-applies committed configuration when the box comes back.
Install one of the two units and the box boots into its own
configuration; install neither and every reboot is effectively a factory
reset with the operator's intent left behind in `show commits`.

```sh
# systemd
systemctl enable einheit-s5-boot.service

# BusyBox init — S39 runs immediately before Buildroot's S40network, so
# br0 and the enslaved ports exist before anything addresses br0.
chmod 755 /etc/init.d/S39einheit-s5
```

Both run `einheit_s5 --apply-boot`, which:

1. builds the switch fabric — `br0` with `vlan_filtering=1`, `lan1..lan4`
   enslaved, `lan5` left routed as the uplink, the DSA conduit up —
   idempotently;
2. re-applies the newest commit's configuration through the same
   `S5Backend::Apply` a `commit` uses;
3. reconciles: intent wins for every path the commit carried, the box
   fills in the rest.

An unconfirmed `commit confirmed` window is **reverted**, not carried
forward: the oneshot has no live timer to fire the auto-revert later, so
carrying the window would quietly make an unconfirmed configuration
permanent. Boot is the confirm deadline.

Both are ordered **before sshd and getty**, not merely before
`multi-user.target`: ordering against a target does not order against the
units pulled into it, so without naming sshd explicitly systemd starts it
concurrently and an operator can log into a switch that has no bridge and
no VLANs yet. On BusyBox init the `S39` prefix gives the same guarantee
against Buildroot's `S50sshd`.

The systemd unit is deliberately **not** ordered before
`network-pre.target`. That reads like the right thing ("before anything
touches the interfaces") and is a trap: `network-pre` is a sysinit-phase
target while the unit is `WantedBy=multi-user.target`, which systemd
reaches only after sysinit — an ordering cycle. systemd breaks a cycle by
deleting a job, and it picked this unit on 2 of 50 power cuts during
Phase 0 testing, booting the box with no fabric and no restored config,
silently. If boot-restore ever appears to be skipped, that is the first
thing to check:

```sh
journalctl -b | grep -E "ordering cycle|deleted to break"
systemd-analyze verify /etc/systemd/system/einheit-s5-boot.service
```

Exit code 0 means restored (or "nothing committed yet"); non-zero means
the fabric or the apply failed, and the box is running whatever it
powered on with. Check `journalctl -u einheit-s5-boot` or the console,
then `show fabric` and `show commits`.

There is deliberately no retry: re-running a half-applied hardware write
is how a bad boot becomes a bad box.

### Ordering caveat

DSA user ports appear asynchronously as the switch driver probes. If
`--apply-boot` runs before a port exists, the fabric bootstrap skips it
and reports it as absent rather than failing — `show fabric` names the
absent ports, and the next commit enslaves them. On an image where this
races, order the unit after the driver's udev settle.

## Verifying a deployment

```
show fabric        # bridge up, vlan_filtering yes, 4 members enslaved
show commits       # history survived the reboot
show config        # running config matches the newest commit
show status        # lock_holder=<none> on a fresh boot
```

`test/vm_integration.sh` drives all of this over ssh against a
disposable target, including power-cycling the fabric behind confd's
back and re-running `--apply-boot`.
