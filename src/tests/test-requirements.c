// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Exercises the firmware requirement checks:
 *
 *   make test-requirements && ./test-requirements
 *
 * These gate the download, and nothing in the live index carries a
 * requirement, so this is the only thing that runs them.
 *
 * Built with VERSION set to a known value, min_gui being a comparison
 * against the app's own compiled-in version.
 *
 * The min_kernel cases go through online_min_kernel_error(), which
 * takes the release rather than reading uname(), so they can be
 * asserted against kernels this machine is not running.
 */

#include <stdio.h>
#include <string.h>

#include "../online-update.h"

static int failures;

/* want is a substring the message must contain, or NULL to require that
 * the requirement is met and nothing is reported.
 */
static void check(
  const char                   *desc,
  const struct online_firmware *firmware,
  const char                   *fcp_server_version,
  const char                   *want
) {
  char *error =
    online_firmware_requirement_error(firmware, fcp_server_version);

  gboolean ok = want
    ? error && strstr(error, want) != NULL
    : error == NULL;

  printf(
    "%-46s %s%s%s\n",
    desc, ok ? "ok" : "FAIL",
    ok || !error ? "" : ": ", ok || !error ? "" : error
  );

  if (!ok)
    failures++;

  g_free(error);
}

// want is TRUE if the release should be accepted
static void check_kernel(
  const char *desc,
  const char *release,
  char      **min_kernel,
  gboolean    want
) {
  char *error = online_min_kernel_error(release, min_kernel);
  gboolean ok = want ? error == NULL : error != NULL;

  printf("%-46s %s\n", desc, ok ? "ok" : "FAIL");

  if (!ok)
    failures++;

  g_free(error);
}

static void check_kernels(void) {
  /* A fix in mainline from 6.10, backported to the 6.8 branch at .12
   * and the 6.9 branch at .5. A branch is held to its own entry, so
   * being in 6.8.12 says nothing about 6.9.0.
   */
  char *backported[] = { "6.8.12", "6.9.5", "6.10", NULL };

  check_kernel("branch entry: below it",
               "6.8.11", backported, FALSE);
  check_kernel("branch entry: at it",
               "6.8.12", backported, TRUE);
  check_kernel("branch entry: above it",
               "6.8.20", backported, TRUE);
  check_kernel("later branch is not covered by an earlier one",
               "6.9.0", backported, FALSE);
  check_kernel("that branch's own entry",
               "6.9.5", backported, TRUE);
  check_kernel("mainline entry",
               "6.10", backported, TRUE);
  check_kernel("newer than every entry",
               "6.11.3", backported, TRUE);
  check_kernel("older than every entry",
               "6.7", backported, FALSE);

  /* A branch with no entry never received the backport, so it is held
   * to the highest entry however close to one it looks
   */
  char *gapped[] = { "6.8.12", "6.10", NULL };

  check_kernel("unlisted branch between two entries",
               "6.9.7", gapped, FALSE);

  // A single bare entry is the ordinary "this release and later"
  char *floor[] = { "6.8", NULL };

  check_kernel("bare entry: the release itself", "6.8", floor, TRUE);
  check_kernel("bare entry: a later patch", "6.8.9", floor, TRUE);
  check_kernel("bare entry: a later branch", "7.1", floor, TRUE);
  check_kernel("bare entry: an earlier release", "6.7.9", floor, FALSE);

  /* The highest entry is searched for rather than assumed last, so a
   * list in the wrong order still refuses what it cannot vouch for
   */
  char *unsorted[] = { "7.1", "6.8.12", NULL };

  check_kernel("out of order: unlisted branch below the highest",
               "6.9", unsorted, FALSE);
  check_kernel("out of order: branch entry still honoured",
               "6.8.12", unsorted, TRUE);
  check_kernel("out of order: above the highest",
               "7.2", unsorted, TRUE);

  // A release string carries a distro suffix
  check_kernel("distro suffix", "7.1.5-201.fc44.x86_64", floor, TRUE);

  // Unusable input is reported rather than passed
  check_kernel("unparseable release", "banana", floor, FALSE);
}

int main(void) {
  struct online_firmware firmware;

  // No requirements at all: nothing to fail
  memset(&firmware, 0, sizeof(firmware));
  check("no requirements", &firmware, NULL, NULL);
  check("no requirements, server known", &firmware, "1.0.1", NULL);

  // min_fcp_server
  memset(&firmware, 0, sizeof(firmware));
  firmware.min_fcp_server = "1.0.1";

  check("fcp-server newer", &firmware, "1.0.2", NULL);
  check("fcp-server equal", &firmware, "1.0.1", NULL);
  check("fcp-server older", &firmware, "1.0.0", "fcp-server 1.0.1");

  /* A missing control means a server too old to publish one, which
   * fails any requirement by construction
   */
  check("fcp-server absent", &firmware, NULL, "fcp-server 1.0.1");

  /* A dev build compares equal to the release it names, the index
   * publishing only clean releases
   */
  check("fcp-server dev build of the minimum",
        &firmware, "1.0.1.3-g4a26", NULL);

  /* A pre-release is older than the version it precedes, so it does not
   * satisfy a requirement naming that version
   */
  memset(&firmware, 0, sizeof(firmware));
  firmware.min_fcp_server = "1.0";
  check("fcp-server pre-release of the minimum",
        &firmware, "1.0beta1", "fcp-server 1.0");
  check("fcp-server release meeting it", &firmware, "1.0", NULL);

  // Pre-releases of the same version rank among themselves
  firmware.min_fcp_server = "1.0beta10";
  check("fcp-server earlier pre-release",
        &firmware, "1.0beta9", "fcp-server 1.0beta10");
  check("fcp-server equal pre-release", &firmware, "1.0beta10", NULL);
  check("fcp-server later pre-release", &firmware, "1.0beta11", NULL);
  check("fcp-server release beats pre-release", &firmware, "1.0", NULL);

  // An unparseable requirement is treated as no requirement, so that a
  // malformed index cannot block every device
  memset(&firmware, 0, sizeof(firmware));
  firmware.min_fcp_server = "not-a-version";
  check("fcp-server requirement unparseable", &firmware, NULL, NULL);

  /* min_gui, against VERSION which this test compiles as 1.0. No
   * firmware has needed this yet; it ships ahead of the firmware that
   * would, an index field an old client ignores being unenforceable.
   */
  memset(&firmware, 0, sizeof(firmware));
  firmware.min_gui = "0.9";
  check("gui newer than required", &firmware, NULL, NULL);

  firmware.min_gui = "1.0";
  check("gui equal to required", &firmware, NULL, NULL);

  firmware.min_gui = "1.1";
  check("gui older than required",
        &firmware, NULL, "alsa-scarlett-gui 1.1");

  firmware.min_gui = "2.0";
  check("gui well older than required",
        &firmware, NULL, "alsa-scarlett-gui 2.0");

  firmware.min_gui = "not-a-version";
  check("gui requirement unparseable", &firmware, NULL, NULL);

  // The kernel is checked first, so a firmware failing two requirements
  // reports the kernel one; with no kernel requirement, the others show
  memset(&firmware, 0, sizeof(firmware));
  firmware.min_fcp_server = "9.9";
  firmware.min_gui = "9.9";
  check("fcp-server reported before gui",
        &firmware, "1.0", "fcp-server");

  check_kernels();

  printf("\n%s\n", failures ? "FAILURES" : "all passed");

  return failures ? 1 : 0;
}
