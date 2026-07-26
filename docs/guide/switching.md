# Switching features

## The fabric

The switch fabric is built by the box itself at boot: bridge `br0` with VLAN filtering over ports **lan1–lan4**; **lan5 is routed** (kept out of the bridge for the WAN/uplink role). `show fabric` reports the live fabric state — bridge present, members enslaved, anything detached or absent.

## VLANs

Port membership lives under `ports.<port>.vlan.<vid>` with four modes: `untagged-pvid` (access port — untagged in and out, this VID is the port's default), `tagged` (trunk member), `untagged`, and `pvid`. A port configured with any VLANs owns its full set — VIDs on the box but absent from config are removed at commit.

Access + trunk in one commit:

    configure
    set ports.lan1.vlan.10 untagged-pvid
    set ports.lan2.vlan.10 untagged-pvid
    set ports.lan3.vlan.20 untagged-pvid
    set ports.lan4.vlan.10 tagged
    set ports.lan4.vlan.20 tagged
    delete ports.lan1.vlan.1
    show diff
    commit

`show vlans` renders the resulting table.

## Ports

`ports.<port>.enabled` is the administrative state (survives reboots — a shut port stays shut through a power cut). `ports.<port>.{speed,duplex,autoneg}` force link parameters (default auto), `mtu` sets frame size (the CPU-path conduit MTU is maintained automatically), `flow_control` toggles pause frames. `show interfaces` for the summary, `show interfaces detail <port>` for negotiated-vs-configured link state and full counters, `clear counters [port]` to baseline them (counters are operational state: a reboot genuinely resets them).

## Loop protection (spanning tree)

**Rapid spanning tree is on in the factory configuration**, and it is the one feature you should think twice before turning off. A switch with a loop in it does not degrade — it saturates, and it takes the management path with it, so the operator who could fix it is the one who cannot reach it any more.

`stp.mode` is `rstp` (802.1w, the default), `stp` (classic 802.1D, for peers that cannot speak rapid) or `off`. `stp.priority` decides the root election: **lowest wins**, and the value is one of the sixteen legal multiples of 4096 that TAB will offer you. Make the switch at the centre of your topology the root deliberately rather than letting MAC addresses decide it:

    configure
    set stp.mode rstp
    set stp.priority 4096
    commit

`show spanning-tree` answers the two questions you actually have: who is root, and what is each port doing. A port in `discarding` is not broken — it is the spanning tree doing its job, holding one side of a loop open.

Per port, `ports.<port>.stp.cost` (0 derives it from link speed) and `ports.<port>.stp.priority` steer path selection. Two more matter operationally:

- **`edge`** — an access port with a desk on it. An edge port forwards immediately instead of waiting out the listening and learning delays, which is the difference between a phone that boots and one that gives up on DHCP. Set it on ports that face endpoints, never on ports that face switches.
- **`bpdu_guard`** — the other half of the same statement. If a port you declared to be an endpoint port starts receiving spanning-tree messages, somebody has patched a switch into it, and the topology you designed is no longer the one running. The guard takes that port out of service and raises an alarm:

        show system            # alarms: bpdu-guard blocked lan2 — ...
        show spanning-tree     # lan2 ... BLOCKED

  The port stays down until you say otherwise, which is the point — `clear spanning-tree bpdu-guard lan2` returns it to service and is your assertion that the loop is gone.

The three bridge timers (`hello`, `max_age`, `forward_delay`) are tied together by the standard: `2 × (forward_delay − 1) ≥ max_age ≥ 2 × (hello + 1)`. A combination that breaks the relationship is refused at commit, with the constraint you broke in the message, rather than being half-applied. Leave them alone unless you have a reason.

`show spanning-tree statistics` counts BPDUs and state transitions per port — a port whose transition count keeps climbing has a flapping link behind it. `clear spanning-tree statistics` re-baselines them.

Spanning tree only applies to bridged ports; configuring it on the routed uplink is refused. On hardware without mstpd installed, a spanning-tree commit **fails** rather than quietly leaving the network unguarded.

## Neighbours (LLDP)

LLDP is on by default. The switch announces itself on every port and records what it hears, so `show neighbors` answers "what is actually on the other end of this cable" without a site visit:

    show neighbors
    port   chassis             system       their port   mgmt address
    lan2   aa:bb:cc:00:00:07   core-sw      ge-0/0/1     10.0.0.1

`lldp.tx_interval` sets how often we announce (default 30 seconds; a neighbour drops us after four missed announcements). `lldp.port.<port>.enabled` turns it off per port — worth doing on a port facing an untrusted network, since LLDP tells whoever is listening your hostname and management address.

Neighbours age out on the TTL they advertised: a table that still listed a switch you unplugged would be worse than an empty one.

## MAC table

`show mac-table [port]` labels each entry: `dynamic` (learned, ages out after `mac.aging_time` seconds), `static` (configured via `mac.static.<mac>.{port,vlan}`), and the kernel's own `local`/`multicast` entries, which are not configuration and cannot be deleted. `clear mac-table [port]` flushes **learned entries only** — an operational verb never deletes configuration.

## Multicast

IGMP snooping is on by default (`igmp_snooping.enabled`): multicast is pruned to ports that joined the group. On an L2 with no multicast router, enable `igmp_snooping.querier` so membership reports keep flowing. `show igmp-snooping groups` lists current groups.

## PoE

`poe.<port>.enabled` and `poe.<port>.power_limit_mw` control power delivery per port (ports 1–5); `show poe` reports per-port state, class, and drawn power; `poe reset <port>` power-cycles a device. Budget/priority management is planned (WP1.9) but not yet implemented. On hardware without the PoE controller, `poe.*` configuration is rejected at commit rather than silently accepted.
