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
 * The min_kernel cases are not asserted here: the check reads the
 * running kernel through uname(), which a test cannot steer.
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

  printf("\n%s\n", failures ? "FAILURES" : "all passed");

  return failures ? 1 : 0;
}
