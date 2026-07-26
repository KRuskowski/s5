#!/usr/bin/env python3
"""Repeatable switch-configuration scenarios for the s5 CLI.

The s5 analog of f's .pkt corpus, one level up: each scenario in
test/scenarios/ is a whole switch configuration applied through the
CLI ONLY (standing rule: the CLI is the whole product surface) and
verified against the box itself. Between scenarios the runner
resets via `rollback to <anchor>` — so every scenario run also
exercises rollback-to — and verifies the baseline actually came
back. --repeat N proves determinism.

Scenario format (line-based, # comments):

    [config]              # set lines, applied in one candidate
    ports.lan1.vlan.10 untagged-pvid
    [delete]              # delete lines, same candidate
    ports.lan1.vlan.1
    [expect-box]          # shell command :: regex the output must match
    sudo /usr/sbin/bridge vlan show :: lan1\\s+10 PVID
    [expect-cli]          # CLI command :: regex on its rendered output
    show vlans :: 10
    [expect-commit-fail]  # marker: the commit itself must FAIL
    reason-regex

Usage: run_scenarios.py [--host s5-test] [--repeat N] [scenario ...]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCEN_DIR = Path(__file__).parent / "scenarios"


def ssh(host, command, stdin=""):
  return subprocess.run(
      ["ssh", host, command],
      input=stdin,
      capture_output=True,
      text=True,
      timeout=120,
  ).stdout


def cli(host, lines):
  """Drive one CLI session; returns rendered output."""
  script = "\n".join(lines) + "\n"
  return ssh(host, "sudo einheit_s5 2>/dev/null", stdin=script)


def strip_ansi(s):
  return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", s)


def parse_scenario(path):
  section = None
  scn = {"config": [], "delete": [], "expect-box": [],
         "expect-cli": [], "expect-commit-fail": []}
  for raw in path.read_text().splitlines():
    line = raw.strip()
    if not line or line.startswith("#"):
      continue
    m = re.match(r"^\[([a-z-]+)\]$", line)
    if m:
      section = m.group(1)
      if section not in scn:
        raise ValueError(f"{path.name}: unknown section {section}")
      continue
    if section is None:
      raise ValueError(f"{path.name}: content before a section")
    scn[section].append(line)
  return scn


def last_commit_id(host):
  out = strip_ansi(cli(host, ["show commits", "exit"]))
  # Plain-mode rendering inserts semantic markers: `commit_id │ [OK] 7`.
  ids = re.findall(
      r"commit_id\s*[|│]\s*(?:\[[A-Z-]+\]\s*)?(\d+)", out)
  return int(ids[-1]) if ids else 0


def apply_scenario(host, scn):
  """Apply through one candidate; returns (committed, output)."""
  lines = ["configure"]
  for kv in scn["config"]:
    lines.append(f"set {kv}")
  for path in scn["delete"]:
    lines.append(f"delete {path}")
    lines.append("y")
  lines += ["commit", "exit", "exit"]
  out = strip_ansi(cli(host, lines))
  return ("commit_id" in out, out)


class Failures:
  def __init__(self):
    self.count = 0
    self.passed = 0

  def check(self, desc, cond, detail=""):
    if cond:
      self.passed += 1
      print(f"    PASS  {desc}")
    else:
      self.count += 1
      print(f"    FAIL  {desc}")
      if detail:
        print(f"          {detail[:200]}")


def run_scenario(host, name, scn, anchor, baseline, f):
  committed, out = apply_scenario(host, scn)

  if scn["expect-commit-fail"]:
    f.check(f"{name}: commit fails as intended", not committed, out)
    for pattern in scn["expect-commit-fail"]:
      f.check(f"{name}: failure names '{pattern}'",
              re.search(pattern, out) is not None, out)
  else:
    f.check(f"{name}: commit succeeds", committed, out)

  for line in scn["expect-box"]:
    cmd, _, pattern = line.partition("::")
    got = ssh(host, cmd.strip())
    f.check(f"{name}: box: {pattern.strip()}",
            re.search(pattern.strip(), got) is not None, got)
  for line in scn["expect-cli"]:
    cmd, _, pattern = line.partition("::")
    got = strip_ansi(cli(host, [cmd.strip(), "exit"]))
    f.check(f"{name}: cli: {pattern.strip()}",
            re.search(pattern.strip(), got) is not None, got)

  # Reset to the anchor and prove the baseline really came back —
  # every scenario doubles as a rollback-to test.
  out = strip_ansi(
      cli(host, [f"rollback to {anchor}", "y", "exit"]))
  f.check(f"{name}: rollback to anchor", "commit_id" in out, out)
  now = normalize_config(
      strip_ansi(cli(host, ["show config", "exit"])))
  if now != baseline:
    base_set = set(baseline.splitlines())
    now_set = set(now.splitlines())
    detail = "; ".join(
        [f"missing: {l.strip()}" for l in sorted(base_set - now_set)]
        + [f"extra: {l.strip()}" for l in sorted(now_set - base_set)])
  else:
    detail = ""
  f.check(f"{name}: baseline restored", now == baseline, detail)


def normalize_config(show_config_output):
  """Order-independent fingerprint of `show config` tree output."""
  lines = [l.rstrip() for l in show_config_output.splitlines()]
  keep = [l for l in lines
          if l and "@local" not in l and "session" not in l
          and "commands" not in l and "duration" not in l
          and "commits" not in l and "rollbacks" not in l
          and "errors" not in l]
  return "\n".join(sorted(keep))


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--host", default="s5-test")
  ap.add_argument("--repeat", type=int, default=1)
  ap.add_argument("scenarios", nargs="*")
  args = ap.parse_args()

  files = sorted(SCEN_DIR.glob("*.scenario"))
  if args.scenarios:
    files = [p for p in files if p.stem in args.scenarios]
  if not files:
    print("no scenarios found")
    return 1

  f = Failures()
  # Anchor: commit the current state so every reset has a fixed id.
  # The id comes from THIS commit's own response row — never from
  # `show commits`, whose rendering is a display surface.
  out = strip_ansi(cli(args.host, ["configure", "commit", "exit",
                                   "exit"]))
  ids = re.findall(
      r"commit_id\s*[|│]\s*(?:\[[A-Z-]+\]\s*)?(\d+)", out)
  anchor = int(ids[-1]) if ids else 0
  if anchor == 0:
    print("could not establish anchor commit")
    return 1
  baseline = normalize_config(
      strip_ansi(cli(args.host, ["show config", "exit"])))
  print(f"anchor commit {anchor}, {len(files)} scenario(s), "
        f"repeat x{args.repeat}")

  for rep in range(args.repeat):
    for path in files:
      print(f"  == {path.stem} (run {rep + 1})")
      run_scenario(args.host, path.stem, parse_scenario(path),
                   anchor, baseline, f)

  print(f"\npassed {f.passed}, failed {f.count}")
  return 1 if f.count else 0


if __name__ == "__main__":
  sys.exit(main())
