# The configuration model

The s5 is configured like a Junos-class switch: you edit a **candidate**, inspect the difference, and **commit** — the box then applies the whole result to hardware transactionally. Nothing you type takes effect before `commit`, and a failed commit applies nothing.

## Editing

`configure` opens the candidate session and the prompt changes from `>` to `#`. Only one session can hold configure mode: a second operator is refused by name (`configure mode is held by karl (session confd-…)`) and an admin can take it over with `configure force`, which discards the holder's candidate. The lock dies with its session — a crashed or disconnected CLI never leaves configure mode stuck.

Inside the session: `set <path> <value>` and `delete <path>` edit the candidate; TAB completes paths segment by segment and values from the schema; `show diff` shows exactly what commit would do (`+` added, `~` changed with the previous value, `-` removed); `exit` leaves configure mode and discards the candidate (as does `rollback candidate`).

Values are validated twice: at `set` against the schema (types, ranges, enum members) and at commit against the hardware — a candidate the box rejects is applied not at all, never halfway.

## Committing

- `commit` — apply the candidate, record it as a numbered revision, close the session.
- `commit confirmed <minutes>` — apply AND arm an auto-revert: if you do not type `confirm` within the window, the box rolls itself back. This is the anti-lockout tool — use it for any change that could cut off your own access (management address, VLAN of the port you came in on). The timer lives in the box, not your SSH session: disconnecting cannot stop the revert, and if power is cut during the window, the **boot reverts it** (an unconfirmed change never becomes permanent by accident).
- A second `commit` inside the window confirms implicitly — the new commit is now what runs.

## History and rollback

Every commit is durable and listed by `show commits`; `show commit <id>` shows its diff against the previous revision. `rollback previous` re-applies the next-to-last revision (as a NEW commit — history only moves forward); `rollback to <id>` re-applies any revision. Both prompt for confirmation.

## Named configurations and rescue

`save <name>` stores the running configuration as a named file; `show configs` lists them; `load <name> merge|replace` pulls one into a candidate (merge overlays, replace substitutes) — commit applies it. `load factory` stages the shipped factory defaults. `rescue` is a reserved name: `save rescue` marks the current configuration as the known-good fallback and `rollback rescue` returns to it; the rescue file survives factory reset by design.

## Boot behaviour

The box **boots into its committed configuration**: an init oneshot rebuilds the switch fabric and re-applies the last committed revision before login is possible. `show system boot` reports what that boot did — applied revision, per-step timing (fabric, config apply), and crucially whether the restore ran **this** boot at all (it is keyed to the kernel boot id, so a boot where the restore was skipped cannot hide behind an old healthy report). If the box's live state disagreed with committed intent at boot (out-of-band edits), `show system` raises a `config-divergence` row and the audit log records it.

## Audit

Every mutating command — CLI or (future) web UI, they share one engine — lands in the audit log with user, role, command, and outcome, including automatic events (auto-revert, boot restore, divergence).
