// SPDX-FileCopyrightText: 2023-2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scarlett4-firmware.h"

#include "firmware-store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <glib.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

const char *scarlett4_firmware_type_magic[] = {
  [SCARLETT4_FIRMWARE_CONTAINER] = "SCARLBOX",
  [SCARLETT4_FIRMWARE_APP]       = "SCARLET4",
  [SCARLETT4_FIRMWARE_ESP]       = "SCARLESP",
  [SCARLETT4_FIRMWARE_LEAPFROG]  = "SCARLEAP"
};

int firmware_version_cmp(const uint32_t *v1, const uint32_t *v2) {
  for (int i = 0; i < FIRMWARE_VERSION_PARTS; i++) {
    if (v1[i] < v2[i])
      return -1;
    if (v1[i] > v2[i])
      return 1;
  }

  return 0;
}

int firmware_version_from_elem(struct alsa_elem *elem, uint32_t *parts) {
  if (!elem || elem->count != FIRMWARE_VERSION_PARTS)
    return 0;

  long *values = alsa_get_elem_int_values(elem);

  if (!values)
    return 0;

  for (int i = 0; i < FIRMWARE_VERSION_PARTS; i++)
    parts[i] = values[i];

  g_free(values);

  return 1;
}

static int verify_sha256(
  const unsigned char *data,
  size_t length,
  const unsigned char *expected_hash
) {
  unsigned char computed_hash[SHA256_DIGEST_LENGTH];
  SHA256(data, length, computed_hash);
  return memcmp(computed_hash, expected_hash, SHA256_DIGEST_LENGTH) == 0;
}

// Calculate MD5
static void calc_md5(
  const unsigned char *data,
  size_t               length,
  unsigned char       *md5_out
) {
  EVP_MD_CTX   *mdctx;
  const EVP_MD *md;
  unsigned int  md_len;

  mdctx = EVP_MD_CTX_new();
  if (mdctx == NULL) {
    fprintf(stderr, "Failed to create MD5 context\n");
    return;
  }
  md = EVP_md5();
  if (md == NULL) {
    fprintf(stderr, "Failed to get MD5 digest\n");
    EVP_MD_CTX_free(mdctx);
    return;
  }

  EVP_DigestInit_ex(mdctx, md, NULL);
  EVP_DigestUpdate(mdctx, data, length);
  EVP_DigestFinal_ex(mdctx, md5_out, &md_len);

  EVP_MD_CTX_free(mdctx);
}

// Convert magic string to enum
static int firmware_type_from_magic(const char *magic) {
  for (int i = 0; i < SCARLETT4_FIRMWARE_TYPE_COUNT; i++)
    if (strncmp(magic, scarlett4_firmware_type_magic[i], 8) == 0)
      return i;
  return -1;
}

static int read_magic(FILE *file, const char *fn) {
  char magic[8];
  if (fread(magic, sizeof(magic), 1, file) != 1) {
    perror("Failed to read magic");
    fprintf(stderr, "Error reading magic from %s\n", fn);
    return -1;
  }

  return firmware_type_from_magic(magic);
}

// Convert from disk format to memory format
static struct scarlett4_firmware *firmware_header_disk_to_mem(
  int type,
  const struct scarlett4_firmware_header_disk *disk
) {
  struct scarlett4_firmware *firmware = calloc(1, sizeof(struct scarlett4_firmware));
  if (!firmware) {
    perror("Failed to allocate memory for firmware");
    return NULL;
  }

  firmware->type = type;
  firmware->usb_vid = ntohs(disk->usb_vid);
  firmware->usb_pid = ntohs(disk->usb_pid);
  for (int i = 0; i < FIRMWARE_VERSION_PARTS; i++)
    firmware->firmware_version[i] = ntohl(disk->firmware_version[i]);
  firmware->firmware_length = ntohl(disk->firmware_length);
  memcpy(firmware->sha256, disk->sha256, 32);

  return firmware;
}

static struct scarlett4_firmware_container *firmware_container_header_disk_to_mem(
  const struct scarlett4_firmware_container_header_disk *disk
) {
  struct scarlett4_firmware_container *container = calloc(
    1, sizeof(struct scarlett4_firmware_container)
  );
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    return NULL;
  }

  container->usb_vid = ntohs(disk->usb_vid);
  container->usb_pid = ntohs(disk->usb_pid);
  for (int i = 0; i < FIRMWARE_VERSION_PARTS; i++)
    container->firmware_version[i] = ntohl(disk->firmware_version[i]);
  container->num_sections = ntohl(disk->num_sections);

  return container;
}

static struct scarlett4_firmware *read_header(
  FILE       *file,
  const char *fn,
  int         type
) {
  struct scarlett4_firmware_header_disk disk_header;
  size_t read_count = fread(
    &disk_header, sizeof(struct scarlett4_firmware_header_disk), 1, file
  );

  if (read_count != 1) {
    fprintf(stderr, "Error reading firmware header\n");
    return NULL;
  }

  struct scarlett4_firmware *firmware = firmware_header_disk_to_mem(type, &disk_header);

  if (!firmware)
    fprintf(stderr, "Invalid firmware header\n");

  return firmware;
}

static struct scarlett4_firmware *read_header_and_data(
  FILE       *file,
  const char *fn,
  int         type
) {
  struct scarlett4_firmware *firmware = read_header(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware header from %s\n", fn);
    return NULL;
  }

  // Read firmware data
  firmware->firmware_data = malloc(firmware->firmware_length);
  if (!firmware->firmware_data) {
    perror("Failed to allocate memory for firmware data");
    goto error;
  }
  size_t read_count = fread(
    firmware->firmware_data, 1, firmware->firmware_length, file
  );
  if (read_count != firmware->firmware_length) {
    if (feof(file))
      fprintf(stderr, "Unexpected end of file\n");
    else
      perror("Failed to read firmware data");
    fprintf(stderr, "Error reading firmware data from %s\n", fn);
    goto error;
  }

  // Verify the firmware data
  if (!verify_sha256(
    firmware->firmware_data,
    firmware->firmware_length,
    firmware->sha256
  )) {
    fprintf(stderr, "Corrupt firmware (failed checksum) in %s\n", fn);
    goto error;
  }

  // Add the MD5 for ESP firmware
  if (firmware->type == SCARLETT4_FIRMWARE_ESP) {
    calc_md5(
      firmware->firmware_data,
      firmware->firmware_length,
      firmware->md5
    );
  }

  return firmware;

error:
  free(firmware->firmware_data);
  free(firmware);
  return NULL;
}

static struct scarlett4_firmware *read_magic_and_header_and_data(
  FILE       *file,
  const char *fn,
  int         section
) {
  int type = read_magic(file, fn);

  if (type < 0 || type == SCARLETT4_FIRMWARE_CONTAINER) {
    fprintf(
      stderr,
      "Invalid firmware type %d in section %d of %s\n",
      type,
      section + 1,
      fn
    );
    return NULL;
  }

  return read_header_and_data(file, fn, type);
}

static struct scarlett4_firmware_container *read_firmware_container_header(
  FILE       *file,
  const char *fn
) {
  struct scarlett4_firmware_container_header_disk disk_header;

  // Read header
  if (fread(
    &disk_header,
    sizeof(struct scarlett4_firmware_container_header_disk),
    1,
    file
  ) != 1) {
    perror("Failed to read container header");
    fprintf(stderr, "Error reading container header from %s\n", fn);
    return NULL;
  }

  return firmware_container_header_disk_to_mem(&disk_header);
}

static struct scarlett4_firmware_container *read_firmware_container(
  FILE       *file,
  const char *fn
) {
  struct scarlett4_firmware_container *container =
    read_firmware_container_header(file, fn);
  if (!container) {
    fprintf(stderr, "Error reading container header from %s\n", fn);
    return NULL;
  }

  if (container->num_sections < 1 || container->num_sections > 3) {
    fprintf(
      stderr,
      "Invalid number of sections in %s: %d\n",
      fn,
      container->num_sections
    );
    goto error;
  }

  // Allocate memory for sections
  container->sections = calloc(
    container->num_sections, sizeof(struct scarlett4_firmware *)
  );
  if (!container->sections) {
    perror("Failed to allocate memory for firmware sections");
    goto error;
  }

  // Read sections
  for (uint32_t i = 0; i < container->num_sections; i++) {
    container->sections[i] = read_magic_and_header_and_data(file, fn, i);
    if (!container->sections[i]) {
      fprintf(stderr, "Error reading section %d from %s\n", i + 1, fn);
      goto error;
    }
  }

  return container;

error:
  scarlett4_free_firmware_container(container);
  return NULL;
}

struct scarlett4_firmware_container *scarlett4_read_firmware_header(const char *fn) {
  FILE *file = fopen(fn, "rb");
  if (!file) {
    perror("fopen");
    fprintf(stderr, "Unable to open %s\n", fn);
    return NULL;
  }

  int type = read_magic(file, fn);

  if (type < 0) {
    fclose(file);
    return NULL;
  }

  struct scarlett4_firmware_container *container = NULL;

  if (type == SCARLETT4_FIRMWARE_CONTAINER) {
    container = read_firmware_container_header(file, fn);
    fclose(file);
    return container;
  }

  struct scarlett4_firmware *firmware = read_header(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware header from %s\n", fn);
    fclose(file);
    return NULL;
  }

  container = calloc(1, sizeof(struct scarlett4_firmware_container));
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    free(firmware);
    fclose(file);
    return NULL;
  }

  container->usb_pid = firmware->usb_pid;
  container->usb_vid = firmware->usb_vid;
  memcpy(container->firmware_version, firmware->firmware_version,
         sizeof(container->firmware_version));
  container->num_sections = 1;
  container->sections = calloc(1, sizeof(struct scarlett4_firmware *));
  if (!container->sections) {
    perror("Failed to allocate memory for firmware sections");
    free(container);
    free(firmware);
    fclose(file);
    return NULL;
  }

  container->sections[0] = firmware;

  fclose(file);

  return container;
}

struct scarlett4_firmware_container *scarlett4_read_firmware_file(const char *fn) {
  // Open file
  FILE *file = fopen(fn, "rb");
  if (!file) {
    perror("fopen");
    fprintf(stderr, "Unable to open %s\n", fn);
    return NULL;
  }

  // Read magic string
  int type = read_magic(file, fn);

  if (type < 0) {
    fprintf(stderr, "Invalid firmware type\n");
    fclose(file);
    return NULL;
  }

  // Container?
  if (type == SCARLETT4_FIRMWARE_CONTAINER) {
    struct scarlett4_firmware_container *container =
      read_firmware_container(file, fn);
    fclose(file);
    return container;
  }

  // Not a container; read the firmware header and data
  struct scarlett4_firmware *firmware = read_header_and_data(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware from %s\n", fn);
    fclose(file);
    return NULL;
  }

  fclose(file);

  // Put the firmware header and data into a container
  struct scarlett4_firmware_container *container = calloc(
    1, sizeof(struct scarlett4_firmware_container)
  );
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    free(firmware->firmware_data);
    free(firmware);
    return NULL;
  }

  container->usb_pid = firmware->usb_pid;
  container->usb_vid = firmware->usb_vid;
  memcpy(container->firmware_version, firmware->firmware_version,
         sizeof(container->firmware_version));
  container->num_sections = 1;
  container->sections = calloc(1, sizeof(struct scarlett4_firmware *));
  if (!container->sections) {
    perror("Failed to allocate memory for firmware sections");
    free(container);
    free(firmware->firmware_data);
    free(firmware);
    return NULL;
  }

  container->sections[0] = firmware;

  return container;
}

void scarlett4_free_firmware_container(struct scarlett4_firmware_container *container) {
  if (!container)
    return;

  if (container->sections) {
    for (uint32_t i = 0; i < container->num_sections; i++) {
      struct scarlett4_firmware *firmware = container->sections[i];
      if (!firmware)
        continue;

      free(firmware->firmware_data);
      free(firmware);
    }

    free(container->sections);
  }

  free(container);
}

const char *scarlett4_firmware_type_to_string(enum scarlett4_firmware_type type) {
  switch (type) {
    case SCARLETT4_FIRMWARE_CONTAINER: return "container";
    case SCARLETT4_FIRMWARE_APP:       return "App";
    case SCARLETT4_FIRMWARE_ESP:       return "ESP";
    case SCARLETT4_FIRMWARE_LEAPFROG:  return "Leapfrog";
    default:                           return "unknown";
  }
}

/* Record fn in the firmware store, if it holds a usable header.
 * Takes ownership of fn.
 */
void scarlett4_store_firmware(char *fn) {
  struct scarlett4_firmware_container *container =
    scarlett4_read_firmware_header(fn);

  if (!container) {
    g_free(fn);
    return;
  }

  firmware_store_add(
    FIRMWARE_GENERATION_SCARLETT4,
    container->usb_pid,
    container->firmware_version,
    fn
  );

  scarlett4_free_firmware_container(container);
}

const uint32_t *scarlett4_get_best_firmware_version(uint32_t pid) {
  return firmware_store_get_version(FIRMWARE_GENERATION_SCARLETT4, pid);
}

struct scarlett4_firmware_container *scarlett4_get_best_firmware(uint32_t pid) {
  const char *fn = firmware_store_get_filename(
    FIRMWARE_GENERATION_SCARLETT4, pid
  );

  if (!fn)
    return NULL;

  // Read the full firmware file (with data)
  return scarlett4_read_firmware_file(fn);
}

// Find a firmware section by type in a container
static struct scarlett4_firmware *find_section(
  struct scarlett4_firmware_container *container,
  enum scarlett4_firmware_type type
) {
  for (uint32_t i = 0; i < container->num_sections; i++)
    if (container->sections[i]->type == type)
      return container->sections[i];
  return NULL;
}

int scarlett4_is_mid_upgrade(struct alsa_card *card) {

  // Read the full container; the store only holds the file name
  struct scarlett4_firmware_container *container =
    scarlett4_get_best_firmware(card->pid);
  if (!container)
    return 0;

  // Find leapfrog and ESP sections
  struct scarlett4_firmware *leapfrog_fw =
    find_section(container, SCARLETT4_FIRMWARE_LEAPFROG);
  struct scarlett4_firmware *esp_fw =
    find_section(container, SCARLETT4_FIRMWARE_ESP);

  if (!leapfrog_fw || !esp_fw) {
    scarlett4_free_firmware_container(container);
    return 0;
  }

  // Check if leapfrog is loaded (device firmware == leapfrog version)
  int leapfrog_loaded =
    firmware_version_cmp(
      card->firmware_version_4, leapfrog_fw->firmware_version
    ) == 0;

  // Check if ESP needs update
  int need_esp =
    firmware_version_cmp(
      card->esp_firmware_version, esp_fw->firmware_version
    ) != 0;

  fprintf(stderr, "is_mid_upgrade: leapfrog_loaded=%d need_esp=%d\n",
    leapfrog_loaded, need_esp);

  scarlett4_free_firmware_container(container);

  return leapfrog_loaded && need_esp;
}
