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

`/var/run/einheit` holds **generated apply artifacts** — the dnsmasq
configuration, the LLDP daemon's configuration, the mDNS repeater's
argument list, the DHCP lease database, and one marker file per service
recording whether the configuration wants it. All of it is rebuilt from
the committed configuration on every commit and every boot, so a tmpfs
is correct and a wipe is harmless. Nothing here is operator-editable and
nothing here needs to survive a reboot.

## Packages the image must carry

Phase 2 turns several features into "a daemon plus a generated config".
The backend **fails the commit** when the binary is absent rather than
reporting success and doing nothing, so a missing package is a feature
the operator cannot turn on — loudly, which is the intent. What the
image needs:

| Package | Needed by | Absent means |
|---|---|---|
| `mstpd` (+ `mstpctl`, `/sbin/bridge-stp`) | RSTP (WP1.2) | **the box cannot complete its first boot** — `stp.mode rstp` is in the shipped factory configuration |
| `dnsmasq` | DHCP server + DNS forwarder (WP2.3) | `vlans.<vid>.dhcp.*` and `dns.serve` are refused at commit |
| `mdns-repeater` | mDNS reflection (WP2.4) | `mdns.enabled` is refused at commit |
| busybox `ntpd` applet (with `-l`) | time client and server (WP2.5) | `ntp.server` is refused at commit |
| `iproute2` (`ip`, `bridge`) | fabric, VLANs, SVIs, routes | nothing works |
| `ethtool` | port speed/duplex/flow control | those paths fail; the rest is fine |

Buildroot has packages for all of these (`BR2_PACKAGE_MSTPD`,
`BR2_PACKAGE_DNSMASQ`, `BR2_PACKAGE_MDNS_REPEATER`,
`BR2_BUSYBOX_...NTPD`). mstpd is **not** in Debian trixie, so the test
VM builds it from `github.com/mstpd/mstpd` — see the note in
`test/vm_prep.sh`'s companion documentation below.

The LLDP daemon is not a package: it is `einheit_s5 --lldp-daemon`, the
same binary in another mode (see `include/einheit/s5/lldp.h` for why we
own the implementation rather than packaging lldpd). It is started by a
commit and should also be started at boot, after `--apply-boot` has
generated its configuration.

### mstpd and the kernel's user-space STP handshake

`stp.mode rstp` sets `/sys/class/net/br0/bridge/stp_state`, which makes
the kernel run `/sbin/bridge-stp br0 start`. That helper — shipped with
mstpd — starts the daemon and registers the bridge, and only if it
succeeds does the kernel settle on `BR_USER_STP` (stp_state **2**). If
the helper is missing or fails, the kernel silently falls back to its
own 802.1D (stp_state 1), which is not what the operator asked for, so
the backend verifies the resulting state and fails the commit otherwise.
`/etc/bridge-stp.conf` needs no entry: with `MSTP_BRIDGES` empty, mstpd
manages every bridge, which is what we want on a box with one.

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
