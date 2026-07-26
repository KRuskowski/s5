#!/usr/bin/env python3
"""PTY interaction suite for the s5 CLI.

Drives the real binary on a pseudo-terminal and asserts the
operator-facing behaviours that unit tests cannot see: colour
detection, zsh-style menu completion (grid below a persistent
prompt, highlight cycling, Enter-accepts), unique-match completion
appending a space, dotted-path descent, Ctrl-C never ending the
session, strict arity errors, and the dumb-terminal fallback.
Every case here started life as a field report.

Usage: pty_suite.py <path-to-einheit_s5>
"""

import os
import pty
import select
import sys
import time

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/einheit_s5"
PASS = 0
FAIL = 0


class Cli:
  """One CLI instance on a pty."""

  def __init__(self, term="xterm-256color", env_extra=None):
    env = dict(os.environ)
    env["TERM"] = term
    env["EINHEIT_ROLE"] = "admin"
    env.pop("NO_COLOR", None)
    env.update(env_extra or {})
    self.pid, self.fd = pty.fork()
    if self.pid == 0:
      os.execve(BINARY, [BINARY], env)
    self.drain(1.5)

  def drain(self, timeout=0.6):
    out = b""
    end = time.time() + timeout
    while time.time() < end:
      r, _, _ = select.select([self.fd], [], [], 0.15)
      if r:
        try:
          out += os.read(self.fd, 4096)
        except OSError:
          break
    return out.decode("utf-8", "replace")

  def send(self, data, timeout=0.6):
    os.write(self.fd, data.encode())
    return self.drain(timeout)

  def alive(self):
    done, _ = os.waitpid(self.pid, os.WNOHANG)
    return done == 0

  def close(self):
    try:
      os.write(self.fd, b"\x03exit\r")
      self.drain(0.4)
      os.kill(self.pid, 15)
    except (OSError, ProcessLookupError):
      pass
    try:
      os.waitpid(self.pid, 0)
    except ChildProcessError:
      pass


def check(desc, cond, detail=""):
  global PASS, FAIL
  if cond:
    PASS += 1
    print(f"  PASS  {desc}")
  else:
    FAIL += 1
    print(f"  FAIL  {desc}")
    if detail:
      print(f"        {detail[:160]!r}")


def banner_and_color():
  cli = Cli()
  out = cli.send("show version\r", 1.0)
  check("colour escapes on a 256color tty", "\x1b[38" in out, out)
  check("show version runs", "product" in out, out)
  cli.close()


def unique_completion_appends_space():
  cli = Cli()
  cli.send("sho")
  out = cli.send("\t")
  # Completed to `show ` — the NEXT tab must open the subcommand
  # grid, which only happens after the appended space.
  out2 = cli.send("\t")
  check("unique match completes with a space", "version" in out2,
        out2)
  cli.close()


def menu_cycles_below_prompt():
  cli = Cli()
  cli.send("sh")
  t1 = cli.send("\t")
  check("menu appears with highlight", "\x1b[7m" in t1, t1)
  check("grid painted below the line (climbs back up)",
        "\x1b[1A" in t1, t1)
  t2 = cli.send("\t")
  check("tab cycles the highlight", "\x1b[7m" in t2 and "show" in t2,
        t2)
  t3 = cli.send("\x1b[Z")
  check("shift-tab cycles backwards", "\x1b[7m" in t3, t3)
  cli.close()


def enter_accepts_selection():
  cli = Cli()
  cli.send("sh")
  cli.send("\t")
  cli.send("\t")  # highlight on `show`
  out = cli.send("\r", 0.8)
  check("enter accepts instead of executing",
        "error" not in out and "parse" not in out, out)
  out2 = cli.send("version\r", 1.2)
  check("accepted token continues the command", "product" in out2,
        out2)
  cli.close()


def dotted_paths_descend():
  cli = Cli()
  cli.send("configure\r")
  cli.send("set d")
  cli.send("\t")  # unique container -> dns.
  out = cli.send("\t")  # children menu
  check("container descends to its fields",
        "primary" in out and "secondary" in out, out)
  cli.close()


def ctrl_c_never_ends_the_session():
  cli = Cli()
  cli.send("show ver")
  cli.send("\x03", 0.8)
  check("ctrl-c at the prompt clears the line", cli.alive())
  out = cli.send("show version\r", 1.2)
  check("shell alive after prompt ctrl-c", "product" in out, out)
  cli.send("ping 127.0.0.1\r", 0.8)
  cli.send("\x03", 1.5)
  check("ctrl-c mid-command leaves the shell alive", cli.alive())
  out = cli.send("show version\r", 1.2)
  check("shell usable after mid-command ctrl-c", "product" in out,
        out)
  cli.close()


def strict_arity():
  cli = Cli()
  out = cli.send("commit 5\r", 0.8)
  check("stray argument is a parse error",
        "unexpected argument" in out, out)
  cli.close()


def dumb_terminal_falls_back():
  cli = Cli(term="dumb")
  out = cli.send("show version\r", 1.2)
  check("dumb TERM still executes commands", "product" in out, out)
  check("dumb TERM emits no colour", "\x1b[38" not in out, out)
  cli.close()


def no_color_menu_is_plain():
  cli = Cli(env_extra={"NO_COLOR": "1"})
  cli.send("sh")
  out = cli.send("\t")
  check("NO_COLOR menu lists candidates",
        "shell" in out and "show" in out, out)
  check("NO_COLOR menu marks selection without reverse video",
        "\x1b[7m" not in out and ">" in out, out)
  cli.close()


def services_tier_completes():
  """The Phase 2 surfaces have to be reachable by TAB, not just by
  knowing they exist. A schema path an operator cannot discover is a
  path they will never use."""
  cli = Cli()
  cli.send("configure\r")
  cli.send("set vlans.10.")
  out = cli.send("\t")
  check("a VLAN's fields complete", "address" in out and "dhcp" in out,
        out)
  cli.send("\x03", 0.5)
  cli.send("set stp.")
  out = cli.send("\t")
  check("spanning-tree fields complete",
        "priority" in out and "forward_delay" in out, out)
  cli.send("\x03", 0.5)
  # Enum VALUES complete too, which is the whole reason bridge
  # priority is an enum of the sixteen legal values rather than a
  # range with a step nobody can see.
  cli.send("set stp.priority ")
  out = cli.send("\t")
  check("bridge priority offers its legal values",
        "4096" in out and "32768" in out, out)
  cli.close()


def new_show_verbs_complete():
  cli = Cli()
  cli.send("show ")
  out = cli.send("\t")
  check("show offers spanning-tree", "spanning-tree" in out, out)
  check("show offers neighbors", "neighbors" in out, out)
  check("show offers route", "route" in out, out)
  check("show offers dhcp", "dhcp" in out, out)
  cli.close()


def clear_verbs_complete():
  cli = Cli()
  cli.send("clear ")
  out = cli.send("\t")
  check("clear offers the new operational verbs",
        "spanning-tree" in out and "dhcp" in out, out)
  cli.close()


def show_spanning_tree_renders_without_a_daemon():
  """A dev box has no mstpd and no bridge. The verb still has to
  answer — and say which of those is the reason — rather than render
  an empty table or an error."""
  cli = Cli()
  out = cli.send("show spanning-tree\r", 1.5)
  check("show spanning-tree answers on a box without STP",
        "disabled" in out, out)
  check("and says why", "mstpd" in out or "stp.mode" in out, out)
  cli.close()


def main():
  for case in (
      banner_and_color,
      unique_completion_appends_space,
      menu_cycles_below_prompt,
      enter_accepts_selection,
      dotted_paths_descend,
      ctrl_c_never_ends_the_session,
      strict_arity,
      dumb_terminal_falls_back,
      no_color_menu_is_plain,
      services_tier_completes,
      new_show_verbs_complete,
      clear_verbs_complete,
      show_spanning_tree_renders_without_a_daemon,
  ):
    print(f"== {case.__name__}")
    case()
  print(f"\npassed {PASS}, failed {FAIL}")
  return 1 if FAIL else 0


if __name__ == "__main__":
  sys.exit(main())
