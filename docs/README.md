# einheit s5 — operator documentation

Two kinds of pages, with different rules:

- **`reference.md` — generated, never hand-edited.** Produced by `einheit_s5 --dump-docs` from the schema and command tree the binary actually runs, so it cannot disagree with the product. A ctest diffs the checked-in file against the binary's output: adding a config path or verb without regenerating fails the build (`./build/einheit_s5 --dump-docs > docs/reference.md`). To fix wording here, edit the schema help strings / command specs and regenerate — that same text is what `?`, `show schema` and the hint system display.
- **`guide/` — hand-written.** The narrative that cannot be generated: what the configuration model is, how commit-confirmed saves you, how VLANs are meant to be used. Review passes and corrections belong here (and in the schema help strings via the rule above).

Every feature lands with both halves: regenerated reference plus its guide section (workspace acceptance criteria enforce this).
