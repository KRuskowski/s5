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

Root shell was left as `/bin/bash` (stock). We temporarily set it to
the CLI earlier and reverted it — that was the SCP-breaking mistake.

## Deploy / update the binary

```sh
scp build-arm/einheit_s5 \
    root@<ip>:/mnt/UDISK/einheit/bin/einheit_s5
```

The `/usr/bin/cli` symlink already points there, so no other step.

## Login model

- `root` -> bash (system maintenance, UART fallback)
- `admin` -> bash, auto-launches CLI (full access); `exit` -> bash
- `oper` -> bash, auto-launches CLI (monitoring)
- any user: type `cli` to launch the CLI manually

## Full reset to stock

```sh
deluser admin 2>/dev/null; deluser oper 2>/dev/null
rm -f /usr/bin/cli /etc/profile.d/einheit.sh
rm -rf /mnt/UDISK/einheit
# optionally trim /root/.ssh/authorized_keys
```

Or reflash the stock image from the Luckfox SDK — nothing of ours is
in the rootfs image itself, only the runtime changes above.
