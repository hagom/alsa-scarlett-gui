// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/* Enumerate every firmware file in the system firmware directories
 * and in the shared download cache directory, recording each in the
 * firmware store under the generation identified by its magic.
 *
 * Files matching neither generation are reported; a missing directory
 * is not an error.
 */
void firmware_enum_all(void);
