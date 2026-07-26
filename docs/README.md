# einheit s5 — operator documentation

Two kinds of pages, with different rules:

- **`reference.md` — generated, never hand-edited.** Produced by `einheit_s5 --dump-docs` from the schema and command tree the binary actually runs, so it cannot disagree with the product. A ctest diffs the checked-in file against the binary's output: adding a config path or verb without regenerating fails the build (`./build/einheit_s5 --dump-docs > docs/reference.md`). To fix wording here, edit the schema help strings / command specs and regenerate — that same text is what `?`, `show schema` and the hint system display.
- **`guide/` — hand-written.** The narrative that cannot be generated: what the configuration model is, how commit-confirmed saves you, how VLANs are meant to be used. Review passes and corrections belong here (and in the schema help strings via the rule above).
  - [`guide/configuration.md`](guide/configuration.md) — candidate/commit, rollback, saved configurations, boot behaviour.
  - [`guide/switching.md`](guide/switching.md) — the fabric, VLANs, ports, spanning tree, LLDP, MAC table, multicast, PoE.
  - [`guide/services.md`](guide/services.md) — SVIs, routing, DHCP, DNS, mDNS reflection, time, and what a service that is down looks like.

Every feature lands with both halves: regenerated reference plus its guide section (workspace acceptance criteria enforce this).
