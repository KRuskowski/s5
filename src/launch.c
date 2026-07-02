/* einheit-launch — setuid-root CLI launcher.
 *
 * Installed as root:root mode 4755. It authenticates the real
 * caller by uid, maps them to an einheit role, establishes the
 * right privilege level, sanitizes the environment, and execs the
 * management CLI. This is the privilege boundary: the CLI itself
 * then runs as root (for admin) or unprivileged (for operator),
 * and gates commands by EINHEIT_ROLE.
 *
 * Deliberately plain C — small, auditable attack surface for a
 * setuid binary. No dynamic anything beyond libc.
 *
 * Copyright (c) 2026 Einheit Networks
 */

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Absolute path — never trust PATH in a setuid program. */
#define CLI_PATH "/mnt/UDISK/einheit/bin/einheit_s5"

int main(void) {
  uid_t ruid = getuid();
  gid_t rgid = getgid();

  struct passwd *pw = getpwuid(ruid);
  if (!pw || !pw->pw_name) {
    fputs("einheit: cannot resolve caller\n", stderr);
    return 1;
  }
  const char *user = pw->pw_name;

  /* Whitelist: only these accounts may enter the CLI. */
  const char *role;
  int become_root;
  if (strcmp(user, "root") == 0) {
    role = "admin";
    become_root = 1; /* already root */
  } else if (strcmp(user, "admin") == 0) {
    role = "admin";
    become_root = 1;
  } else if (strcmp(user, "oper") == 0) {
    role = "operator";
    become_root = 0; /* read-only: no privilege needed */
  } else {
    fprintf(stderr, "einheit: '%s' is not authorized for the CLI\n",
            user);
    return 1;
  }

  /* Establish privilege. Admin -> full root (ruid=euid=gid=0) so
   * the CLI and its children run cleanly as root. Operator ->
   * drop any setuid privilege back to the real user. */
  if (become_root) {
    if (setgid(0) != 0 || setuid(0) != 0) {
      perror("einheit: cannot become root");
      return 1;
    }
  } else {
    if (setgid(rgid) != 0 || setuid(ruid) != 0) {
      perror("einheit: cannot drop privilege");
      return 1;
    }
  }

  /* Capture a couple of safe values before scrubbing the env. */
  const char *term = getenv("TERM");
  if (!term) term = "xterm";
  char term_buf[64];
  snprintf(term_buf, sizeof(term_buf), "TERM=%s", term);

  /* Scrub the environment — a setuid program must not inherit
   * caller-controlled variables (LD_*, IFS, PATH, ...). Rebuild
   * a minimal, known-good env for the CLI. */
  char user_buf[128];
  char role_buf[64];
  snprintf(user_buf, sizeof(user_buf), "EINHEIT_USER=%s", user);
  snprintf(role_buf, sizeof(role_buf), "EINHEIT_ROLE=%s", role);

  char *newenv[] = {
      "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
      become_root ? "HOME=/root" : "HOME=/home/oper",
      term_buf,
      user_buf,
      role_buf,
      NULL,
  };

  execle(CLI_PATH, "einheit-cli", (char *)NULL, newenv);
  perror("einheit: exec CLI failed");
  return 1;
}
