// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

/* Exercises the app's own version comparison:
 *
 *   make test-client-version && ./test-client-version
 *
 * Covers a release against a beta of it, a development build against
 * the release it describes, and the interaction of the two suffixes.
 */

#include <stdio.h>

#include "../online-update.h"

static int failures;

/* Compare both ways round. An ordering that lacks antisymmetry would
 * report an update in one direction while denying it in the other.
 */
static void check(const char *a, const char *b, int want) {
  int got = online_client_version_cmp(a, b);
  int rev = online_client_version_cmp(b, a);

  got = got < 0 ? -1 : got > 0 ? 1 : 0;
  rev = rev < 0 ? -1 : rev > 0 ? 1 : 0;

  gboolean ok = got == want && rev == -want;

  printf(
    "%-22s %2s %-22s %s\n",
    a, want > 0 ? ">" : want < 0 ? "<" : "==", b, ok ? "ok" : "FAIL"
  );

  if (!ok)
    failures++;
}

int main(void) {

  // A release beats any pre-release of it
  check("1.0", "1.0beta9", 1);
  check("1.0.1", "1.0beta9", 1);
  check("1.0", "1.0beta9-54-gafa28aa", 1);

  // Betas among themselves, where a plain string compare puts 10 first
  check("1.0beta10", "1.0beta9", 1);
  check("1.0beta9", "1.0beta9", 0);

  /* A development build is newer than the release it describes: the
   * git-describe suffix counts commits forward, the opposite direction
   * to a pre-release tag
   */
  check("1.0beta9-54-gafa28aa", "1.0beta9", 1);
  check("1.0.1-1-gabc", "1.0.1", 1);
  check("1.0beta9-55-g695e6c6", "1.0beta9-54-gafa28aa", 1);

  /* The comparison the running app performs: the index publishes clean
   * releases, and a development build of a beta is still older than the
   * release that beta precedes
   */
  check("1.0", "1.0beta9-55-g695e6c6", 1);
  check("1.0beta10", "1.0beta9-55-g695e6c6", 1);

  // Plain numeric ordering
  check("1.0.1", "1.0", 1);
  check("2.0", "1.9.9.9", 1);
  check("0.4.0", "0.3.9", 1);
  check("1.0", "1.0", 0);

  /* Omitted components are zero, and components past the fourth stay
   * part of the version, so they do not read as a pre-release tag
   */
  check("1.0.0.0", "1.0", 0);
  check("1.0.0.0.5", "1.0", 0);

  // A version that parses to nothing is older than one that does
  check("", "1.0", -1);
  check("unknown", "1.0", -1);

  printf("\n%s\n", failures ? "FAILURES" : "all passed");

  return failures ? 1 : 0;
}
