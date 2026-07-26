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

## MAC table

`show mac-table [port]` labels each entry: `dynamic` (learned, ages out after `mac.aging_time` seconds), `static` (configured via `mac.static.<mac>.{port,vlan}`), and the kernel's own `local`/`multicast` entries, which are not configuration and cannot be deleted. `clear mac-table [port]` flushes **learned entries only** — an operational verb never deletes configuration.

## Multicast

IGMP snooping is on by default (`igmp_snooping.enabled`): multicast is pruned to ports that joined the group. On an L2 with no multicast router, enable `igmp_snooping.querier` so membership reports keep flowing. `show igmp-snooping groups` lists current groups.

## PoE

`poe.<port>.enabled` and `poe.<port>.power_limit_mw` control power delivery per port (ports 1–5); `show poe` reports per-port state, class, and drawn power; `poe reset <port>` power-cycles a device. Budget/priority management is planned (WP1.9) but not yet implemented. On hardware without the PoE controller, `poe.*` configuration is rejected at commit rather than silently accepted.
