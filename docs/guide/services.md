# Services: routing, DHCP, DNS, discovery, time

Everything on this page turns the s5 from a switch into the thing at the centre of a small network. It is all optional and all off until you ask for it — a switch that started handing out addresses on delivery would be a menace on somebody else's network.

## The shape of it

A VLAN with an **address** becomes routable. Give it one and the switch grows an interface in that VLAN (`br0.<vid>`), which is what lets it be a gateway, serve DHCP, answer DNS and reflect discovery there. Without an address a VLAN is still a perfectly good switched VLAN; it just has no presence of its own.

That ordering is worth internalising, because most refusals on this page are it in disguise: DHCP needs an address, mDNS reflection needs an address, inter-VLAN routing needs an address at both ends.

## VLAN identity and switch addresses

    configure
    set vlans.10.name office
    set vlans.10.address 10.10.0.1/24
    set vlans.20.name guest
    set vlans.20.address 10.20.0.1/24
    commit

`show vlans` now reads as one row per VLAN — what it is called, what the switch's address in it is, and which ports are in it:

    vid   name     address        members
     10   office   10.10.0.1/24   lan1(u,pvid) lan4(t)
     20   guest    10.20.0.1/24   lan2(u,pvid) lan4(t)

The **name** is pure configuration — there is nothing on the box to read it back from, so it lives in the commit and survives reboots the same way every other setting does. Deleting a VLAN's address removes its interface; the VLAN itself stays.

## Routing

`routing.enabled` is the master switch for forwarding between VLANs and out of the uplink. Routes with forwarding off are a configuration that looks right and moves no packets, so `show route` puts the forwarding state at the top of the table rather than hiding it in another command.

    set routing.enabled true
    set interfaces.wan.gateway 203.0.113.1
    set routing.static.branch.prefix 192.168.44.0/24
    set routing.static.branch.via 10.20.0.9

There are two ways to name a default route — an interface's `gateway`, or a static route to `0.0.0.0/0` — and setting both is refused rather than resolved arbitrarily.

**The switch owns only the routes it installed.** `show route` marks those `config`; a route the kernel derived from an address, one a DHCP lease brought in, or one your network configuration put there is left alone, and deleting a static route from the configuration removes it from the box on the next commit.

## DHCP

A pool belongs to a VLAN, and the switch has to be in the subnet it is serving:

    set vlans.10.dhcp.enabled true
    set vlans.10.dhcp.range_start 10.10.0.100
    set vlans.10.dhcp.range_end 10.10.0.200
    set vlans.10.dhcp.lease_time 720
    set vlans.10.dhcp.static.aa:bb:cc:dd:ee:ff.ip 10.10.0.50

`lease_time` is in minutes. `gateway` and `dns` default to the switch itself and are there for the cases where they should not be. `vlans.<vid>.dhcp.static.<mac>.ip` is a reservation: that MAC always gets that address.

Pools are per VLAN and stay that way — a client in VLAN 20 is served out of VLAN 20's pool or not at all. A range outside the VLAN's own subnet is refused at commit, because the alternative is a DHCP server that fails to start after the rest of the box has already been reconfigured.

`show dhcp server` lists the pools and how much of each is in use; `show dhcp leases` shows who holds what and for how much longer; `clear dhcp lease <ip|mac>` hands one address back. That last one briefly restarts the server, because the lease database is only read at startup — a quiet edit that left the server holding the old lease would report success and change nothing.

## DNS

The same daemon forwards DNS when you ask it to:

    set dns.serve true
    set dns.local_domain office.lan
    set dns.primary 9.9.9.9
    set dns.secondary 1.1.1.1

`dns.primary` / `dns.secondary` are the switch's own resolvers *and* the upstreams it forwards to. `dns.local_domain` is the domain the switch answers for itself; names it does not know go upstream.

## mDNS reflection

Discovery protocols deliberately do not cross subnets, which is correct and also why the printer in the printer VLAN is invisible from the laptop VLAN. Reflection repeats mDNS between the VLANs you name:

    set mdns.enabled true
    set mdns.reflect.10 true
    set mdns.reflect.20 true

At least two VLANs, each with an address, up to five. Reflecting into one VLAN is refused: it would start a repeater that repeats nothing while reporting itself healthy.

## Time

`ntp.server` points the switch at an upstream. `ntp.serve` additionally makes it answer clients:

    set ntp.server pool.ntp.org
    set ntp.serve true

Serving without a source is refused — a switch handing out time it never synchronised is worse than one handing out none. `show ntp` reports the source, whether we are synchronised, and whether we are serving.

## When a service is not running

Every service on this page is a real daemon, and the switch is honest about them in both directions.

**At commit time**: a feature whose daemon is not installed on the box **fails the commit**. Configuring DHCP on an image without dnsmasq gives you an error, not a clean commit and a phone that never gets an address.

**At any other time**: `show system services` is where you find out that something died after it started.

    show system services
    service         state             role
    mstpd           running           rstp loop protection on br0
    lldp            running           advertising on 5 port(s) every 30s
    dnsmasq         DOWN              2 DHCP pool(s), DNS on
    mdns-repeater   not configured    mDNS reflection between VLANs

`DOWN` means the configuration asks for it and it is not there — the row this command exists for. Re-committing restarts it; init restarts it after a crash.

## Generated configuration is not yours to edit

dnsmasq's configuration file, the LLDP daemon's, and the mDNS repeater's arguments are all **apply artifacts**. They are rewritten from the committed configuration on every commit and every boot, they say so in their first line, and anything you edit into them is gone at the next commit. The CLI is the whole configuration surface; there is deliberately no supported way in underneath it.

That is also why hostile values fail rather than being escaped: these formats have no quoting, so a newline inside a domain name or an address would be a new directive. There is no safe escaping of that — only refusal, at commit, naming the path you set.

## Not locking yourself out

Changing the address, VLAN, route or port your own session is riding on is a legitimate thing to do and a well-known way to lose a switch. The s5 will not stop you, but it will tell you before it happens — on `show diff` as well as on `commit`:

    warning: this session reaches the switch on VLAN 10 (10.10.0.1), and
    this commit changes vlans.10.address — consider `commit confirmed 5`

`commit confirmed <minutes>` applies the change and arms an automatic revert: if you do not type `confirm` within the window, the box rolls itself back. The timer lives in the switch, not in your session, so losing the connection is exactly the case it handles — and if the power goes out during the window, the boot reverts it too. See [configuration.md](configuration.md) for the full lifecycle.
