/* ntpd_sim — stand-in for busybox ntpd on the s5 test VM.
 *
 * Debian's busybox build ships no ntpd applet, so the VM cannot
 * exercise the CLI's real NTP apply path (sys::SetNtpServer starts
 * `ntpd -p <server>` and verifies the daemon is up;
 * sys::GetNtpStatus reads the server back from /proc cmdline).
 * This mimics exactly that contract — daemonize, keep the args in
 * the cmdline, run until killed — and keeps no time at all.
 *
 * Build:   gcc -static -O2 -o ntpd ntpd_sim.c
 * Install: /usr/local/sbin/ntpd on the test VM only.
 */
#include <unistd.h>

int main(void) {
  if (daemon(0, 0) != 0) {
    return 1;
  }
  for (;;) {
    pause();
  }
}
