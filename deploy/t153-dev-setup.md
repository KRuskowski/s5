# Luckfox Lume (T153) dev-board setup

The T153 Lume is a **software dev target only** — it runs the stock
Luckfox Buildroot image. Production images target the T113-S3 with a
clean mainline Buildroot (see `deploy/README.md`).

Everything einheit-specific lives under **`/mnt/UDISK/einheit/`** (the
writable data partition), so the rootfs stays essentially stock and a
reflash wipes nothing of ours that we can't restore from this repo.

## What we changed on the stock rootfs (minimal, documented)

| Location | Change | Reset |
|---|---|---|
| `/etc/passwd` | added `admin` (uid 1000), `oper` (uid 1001), shell `/bin/bash` | `deluser admin; deluser oper` |
| `/root/.ssh/authorized_keys` | added khruskowski admin key | remove the line |
| `/usr/bin/cli` | symlink -> `/mnt/UDISK/einheit/bin/einheit_s5` | `rm /usr/bin/cli` |
| `/etc/profile.d/einheit.sh` | `cli` alias + admin/oper auto-launch | `rm /etc/profile.d/einheit.sh` |
| `/mnt/UDISK/einheit/` | all einheit binaries + configs | `rm -rf /mnt/UDISK/einheit` |
| `/home` perms | `chmod 755 /home` | (leave — this is the correct default) |

**GOTCHA (fixed):** stock Luckfox image ships `/home` as `drwx------`
(700, root:root). sshd drops to the target uid to read
`~/.ssh/authorized_keys`, but a non-root user can't traverse into a
700 `/home`, so pubkey auth silently fails ("Could not open authorized
keys: Permission denied" only visible with `sshd -ddd`). Root works
because `/root` is a separate path. Fix: `chmod 755 /home`. The
production Buildroot image must ship `/home` as 755.

**GOTCHA (fixed) — root umask 077:** the T153 root shell has
`umask 077`, so everything created as root came out root-only and
locked out admin/oper. Symptoms: `cli: command not found` and the
CLI auto-launch not firing on admin login. All of these need world
read/exec:
- `/mnt/UDISK/einheit` + `/mnt/UDISK/einheit/bin` -> `chmod 755`
  (traversable)
- `/mnt/UDISK/einheit/bin/einheit_s5` -> `chmod 755` (execable)
- `/etc/profile.d/einheit.sh` -> `chmod 644` (readable — else
  /etc/profile's `for i in /etc/profile.d/*.sh` skips it and neither
  the `cli` alias nor the admin/oper auto-launch loads)

The production Buildroot image must install these with correct perms
(755 dirs/binaries, 644 profile scripts) rather than inheriting a
077 umask.

**GOTCHA — admin `ping` needs unprivileged ICMP:** busybox `ping`
uses raw sockets (needs root/CAP_NET_RAW), so `ping` from the CLI as
admin fails ("permission denied, are you root?"). This image has no
`setcap` and ping is a busybox symlink, so there's no clean dev-board
fix (setuid-ing busybox would give every applet root — don't).
Production image fix: ship **iputils** `ping` (`BR2_PACKAGE_IPUTILS`),
which uses SOCK_DGRAM ICMP and honours
`net.ipv4.ping_group_range` (set to `0 2147483647` in
`/etc/sysctl.d/99-einheit-ping.conf`). Then admin ping works with no
setuid/caps.

Root shell was left as `/bin/bash` (stock). We temporarily set it to
the CLI earlier and reverted it — that was the SCP-breaking mistake.

## Deploy / update the binary

```sh
scp build-arm/einheit_s5 \
    root@<ip>:/mnt/UDISK/einheit/bin/einheit_s5
```

The `/usr/bin/cli` symlink already points there, so no other step.

## Login + privilege model

`cli` is a **setuid-root launcher** (`einheit-launch`, installed
`root:root` mode `4755`) — the single privilege boundary. It resolves
the real caller by uid, whitelists accounts, sets `EINHEIT_USER`/
`EINHEIT_ROLE`, scrubs the environment, and execs the CLI:

| Caller | Privilege after launcher | Role |
|---|---|---|
| `root`  | root | admin |
| `admin` | **becomes root** (`setuid(0)`) | admin — all commands, incl. `shell` |
| `oper`  | **drops to uid 1001** (unprivileged) | operator — read-only; `set`/`shell` gated |
| other   | rejected ("not authorized") | — |

So `set hostname`, `reboot`, etc. execute as root for admin, while
operator gets a read-only CLI running unprivileged (defence in depth:
even a role-gating bug can't touch the system because the process
isn't root). `admin`/`oper` login `exec`s the launcher, so exiting
the CLI logs out (Juniper-style); admin reaches a root shell via the
in-CLI `shell` command. `root` login -> bash (dev/UART maintenance).

Install (already done on the T153; production image must replicate):
```sh
install -o root -g root -m 4755 einheit-launch \
    /mnt/UDISK/einheit/bin/einheit-launch
ln -sf /mnt/UDISK/einheit/bin/einheit-launch /usr/bin/cli
```

## Full reset to stock

```sh
deluser admin 2>/dev/null; deluser oper 2>/dev/null
rm -f /usr/bin/cli /etc/profile.d/einheit.sh
rm -rf /mnt/UDISK/einheit
# optionally trim /root/.ssh/authorized_keys
```

Or reflash the stock image from the Luckfox SDK — nothing of ours is
in the rootfs image itself, only the runtime changes above.
