# s5 Software Roadmap — Config Skeleton to Managed Switch + Edge Services

What S501 needs to ship as a product, sequenced so every phase lands on the
existing management plane (confd lifecycle, schema-driven config, four test
layers) instead of beside it. Written 2026-07-26 against the honest current
state; the companion gap analysis is in the "Current state" section, the
decisions Karl owes are collected at the bottom.

## Product statement

**einheit s5 (SKU S501): 5-port gigabit managed PoE switch and small-site edge
appliance.** Line-rate L2 in the KSZ9477 ASIC; the T113-S3 management plane
adds transactional configuration (CLI + web), VLANs, PoE management, and an
*edge services tier* — DHCP, DNS/mDNS, NAT and stateful filtering via f — so a
small site needs one DIN-rail box, not three.

The s1xx family constraint stands: s5 is s100's software on cheaper hardware.
Everything in this roadmap that isn't PoE- or ASIC-specific must stay portable
(schema + backend seams, no s5-only assumptions in shared code).

## Architecture facts that drive the design

- **T113-S3: 2× Cortex-A7, 128 MB SiP DDR3, SPI NAND.** Every service is
  budgeted; there is no room for "install the big daemon" answers. Rough
  working budget: kernel + base 40 MB, management plane 15 MB, services tier
  30 MB, f 20 MB, 20 MB free.
- **The ASIC switches; the CPU sees only CPU-port traffic.** Intra-VLAN
  traffic between lan ports never touches Linux. Consequence: there are two
  distinct enforcement points, and the roadmap treats them separately:
  1. **CPU path** (inter-VLAN routing, NAT to WAN, traffic to/from the box
     itself): full f — XDP/conntrack/NAT/zones — applies here. CPU-path
     throughput is bounded by the A7s, which is acceptable: the ASIC carries
     the line-rate load.
  2. **Switched path**: only the KSZ9477's own per-port ACL tables can filter
     at line rate. Small rule count, stateless matches. This is what
     **f-light** concretely means: an FWL subset compiled into ASIC ACL
     entries, not XDP.
- **f today** (post v0.4 phases): stateful matching, conntrack, NAT
  (SNAT/masquerade/DNAT), zones with daemon wiring, VLAN matching, multi-def +
  pipeline splitter. NAT-via-f is reuse, not new engineering — the work is
  integration and packaging, plus the open question of where FWL compiles
  (the Python compiler does not belong on 128 MB of NAND).

## Current state (2026-07-26, honest)

Works, with the four-layer harness green (unit 261+9, schema exerciser,
lifecycle fuzzer 5×400 ops, VM integration 41 + pty 19):

- confd management plane end to end: candidate/commit/commit-confirmed
  auto-revert/rollback, durable history, audit, `show diff` (tree view).
- Config surface: hostname, DNS, NTP, per-port admin state + addresses,
  802.1Q membership per port, PoE enable/limit.
- CLI feel: zsh-style menu completion, schema-driven validation, crash
  regime (signals, supervisor, Ctrl-C).

Known-untested: real silicon (KSZ9477 DSA and TPS23861 register code have
zero hardware coverage — the VM fakes both), concurrency, scale, soak,
security depth. The harness verifies the config plane on a stand-in box.

Structural holes (Phase 0 exists because of these):

- **No startup-config.** Nothing re-applies committed config at boot; the
  runtime adopts whatever the box wakes up as. Reboot = factory behaviour
  with the intent stranded in history.
- **No fabric bootstrap.** br0 + vlan_filtering + enslavement was hand-built
  on the test VM; the product cannot construct its own switch fabric.
- **No edit locking** (MANAGEMENT_PLANE.md requirement, unimplemented).
- No config export/import, no factory reset.

## Phase 0 — Structural: a switch that survives a reboot

1. **Boot-restore.** An `--apply-boot` mode in the s5 binary (or a separate
   oneshot) run from init before login is possible: load confd state, apply
   the committed running config through S5Backend, THEN reconcile reality.
   Ordering with commit-confirmed recovery matters: an expired window must
   revert, not re-apply. Framework change: `Runtime` grows an explicit
   `ApplyRunningAtBoot()` entry so every s1xx product gets it.
2. **Fabric bootstrap as config.** The bridge is not preexisting environment;
   it is config. Backend owns creating br0 (vlan_filtering=1), enslaving
   lan1..lan5, and the CPU-port conduit bring-up, idempotently, before any
   port/VLAN apply. New schema root `fabric.*` only if anything is genuinely
   configurable (likely not v1 — hardcode the s5 topology in the backend).
3. **Edit locking.** confd: second `configure` names the holder and (admin)
   can `configure force` to steal; lock dies with the session. Framework
   work, fuzzer gets a two-session op mix.
4. **Config file surface.** `show config | save` → single file (the flat
   kv format already in the store); `load merge|replace` into a candidate.
   Enables backup/restore and factory reset (`load factory` = shipped
   defaults file + commit).

Exit gate: pull power mid-soak on the bench switch 50 times; box always
boots into its committed config; fuzzer extended with restart+boot-apply ops.

## Phase 1 — L2 completeness (the "managed switch" cut)

In: **RSTP** (kernel bridge STP + KSZ9477 offload where the driver allows;
loops must not melt the network), **LLDP** (tx+rx, `show neighbors`; small
own implementation or lldpd — budget decides), **port speed/duplex/autoneg/
MTU**, **static MAC entries + aging time**, **port mirroring** (ASIC mirror
via tc/DSA offload), **storm control** (ASIC rate limiters), **IGMP
snooping** (bridge feature, on by default).

Out (deliberately, revisit on demand): LACP (5 ports; who aggregates?),
802.1X (small-site reality: rarely deployed; the schema must not preclude a
later `ports.<p>.dot1x.*`), per-VLAN STP.

Schema sketch: `ports.<p>.{speed,duplex,mtu,mirror-to,storm.*}`,
`stp.{enabled,priority}`, `mac.{aging,static.<addr>}`, `lldp.enabled`.

## Phase 2 — System services tier

Design rule: services are config-plane citizens — schema + confd lifecycle +
`show` state + all four test layers — never "edit this daemon's conf file".

- **SVIs**: `vlans.<vid>.{name,address}` → `br0.<vid>` interfaces. The
  routing/NAT/services anchor; today's `interfaces.br0.address` generalises.
- **DHCP server** per VLAN: `vlans.<vid>.dhcp.{range,lease,gateway,dns,
  static.<mac>}`. Engine: dnsmasq (one small binary, battle-tested,
  dhcp+dns+ra in ~1 MB) — generated config + SIGHUP from the backend.
- **DNS**: forwarder/cache (same dnsmasq instance), local host records from
  DHCP leases, upstream from `dns.*`.
- **mDNS reflector** across chosen VLANs: `mdns.reflect = [vids]` —
  avahi-reflector is heavy; evaluate a minimal reflector.
- **DHCP/DNS client roles** already exist (interfaces.dhcp, dns.*); WAN
  gains `interfaces.wan.gateway` + default-route handling.
- **NTP**: existing client; optionally serve LAN via the same chrony/busybox
  decision as the target image.

Exit gate: VM suite grows a services section (lease actually served on a
VLAN, name resolves, mDNS crosses exactly the configured VLANs).

## Phase 3 — f integration: NAT + firewall ("f-light" and f-full)

- **CPU-path f (f-full).** fd runs on the box; zones map from VLANs/SVIs +
  wan; NAT policies (`masquerade wan`, DNAT port-forwards) and inter-VLAN
  rules ride f's existing NAT/zone machinery. s5 schema references a policy
  bundle: `firewall.{enabled,bundle,zones.<name>.members}` plus first-class
  sugar for the common cases (`firewall.nat.masquerade=wan`,
  `firewall.forward.<n>.{proto,port,to}`) that compile to FWL underneath.
- **Compile placement decision**: FWL compiles off-box (workstation/CI
  produces a signed bundle, box verifies + fd hot-loads — matches fd's
  bundle model) — v1. On-box compile is out for 128 MB.
- **ASIC ACLs (f-light).** Spec an FWL subset (stateless 5-tuple matches,
  per-port, N≤16 entries/port) compiled to KSZ9477 ACL tables for line-rate
  filtering of switched traffic. New fd-style backend or S5Backend applier —
  spec first with hone sign-off, per house methodology.
- **XDP feasibility spike comes first**: verify XDP (generic mode is
  acceptable) + conntrack on the T113 vendor/mainline kernel and the DSA
  conduit before committing the phase.

## Phase 4 — Management surfaces

- **Web UI**: einheit-ui adapter for s5 over the CommandDriver seam (same
  chokepoint/audit as CLI — the framework side already exists). Dashboard:
  ports/PoE/power budget, VLAN matrix, DHCP leases, firewall counters.
- **SNMP read-only** (ifTable, bridge MIB, PoE MIB) — small agent, budget
  decides; SNMP write is out.
- **Remote syslog** (`logging.remote`), audit log rotation.
- **Firmware update**: A/B rootfs slots on the NAND + U-Boot fallback;
  `system update <url>` + `commit confirmed`-style auto-fallback on failed
  boot. This is its own mini-project and gates real deployments.

## Phase 5 — Security hardening

Mgmt-plane ACL (who may reach CLI/UI, which VLAN), per-port MAC limiting
(ASIC), DHCP snooping/guarding once the DHCP server exists, real user store
+ roles beyond root/operator, and a hostile-input pass over the service
configs (every value that reaches a shell or a generated conf file).

## Testing implications (standing rule)

A phase is done when: schema exerciser covers its paths (automatic),
backend unit tests fake its box interface, the lifecycle fuzzer knows any
new op, the VM suite verifies it against a real kernel, and — new tier —
**bench-switch tests** cover what the VM cannot: ASIC ACLs, PoE silicon,
DSA offloads, boot-restore under power pulls. The takt pipeline (clone VM →
run suites → destroy; nightly soak) plus a claimed bench target is the CI
end-state.

## Sequencing and rough effort

| Phase | Contents | Effort | Hard dependency |
|---|---|---|---|
| 0 | boot-restore, fabric bootstrap, edit lock, save/load | 1–2 wk | none — first |
| 1 | RSTP, LLDP, port params, mirror/storm, static MAC | 2–3 wk | 0 |
| 2 | SVIs, DHCP/DNS/mDNS, WAN routing | 2 wk | 0 (SVIs), parts of 1 |
| 3 | f NAT/zones on CPU path; ASIC ACL subset; XDP spike | 3–4 wk | 2 (SVIs/zones) |
| 4 | web UI, SNMP-ro, syslog, A/B firmware update | 3 wk | 0; UI any time |
| 5 | mgmt ACL, port security, snooping, users | 2 wk | 2, 3 |

## Decisions needed (Karl)

1. **Is s5 a router?** Phase 2/3 assume yes-at-the-edge (SVIs, NAT to WAN).
   If s5 is L2-only and routing is f.appliance's job, Phases 2–3 shrink a lot.
2. **f bundle authorship**: are firewall policies written as FWL by the
   operator (power tool) or only through the schema sugar (appliance)? v1
   proposal: sugar only, FWL escape hatch behind a flag.
3. **Feature cut sign-off** for Phase 1's out-list (LACP, 802.1X out).
4. **dnsmasq vs own minimal services** — proposal: dnsmasq v1, measure.
5. **Update transport** (USB/local file vs network pull) for Phase 4.
6. **Bench target**: which board becomes the claimed hardware test target
   (T113 EVB now, S501 proto later)?
