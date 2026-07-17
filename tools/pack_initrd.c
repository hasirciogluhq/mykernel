/* Host tool: pack .kmod files into a simple initrd image. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITRD_MAGIC 0x44525249u /* 'IRRD' */
#define INITRD_NAME_MAX 32
#define INITRD_MAX_FILES 64

typedef struct {
  char name[INITRD_NAME_MAX];
  uint32_t offset;
  uint32_t size;
} initrd_file_t;

typedef struct {
  uint32_t magic;
  uint32_t count;
  initrd_file_t files[INITRD_MAX_FILES];
} initrd_header_t;

static const char *basename_of(const char *path) {
  const char *p = path;
  while (*path) {
    if (*path == '/' || *path == '\\')
      p = path + 1;
    path++;
  }
  return p;
}

int main(int argc, char **argv) {
  initrd_header_t hdr;
  FILE *out;
  int i;
  uint32_t offset;
  uint8_t *blobs[INITRD_MAX_FILES];
  uint32_t sizes[INITRD_MAX_FILES];

  if (argc < 3) {
    fprintf(stderr, "usage: %s <out.img> <file.kmod|.mke>...\n", argv[0]);
    return 1;
  }
  if (argc - 2 > INITRD_MAX_FILES) {
    fprintf(stderr, "too many files\n");
    return 1;
  }

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = INITRD_MAGIC;
  hdr.count = (uint32_t)(argc - 2);
  offset = (uint32_t)sizeof(hdr);

  for (i = 0; i < (int)hdr.count; i++) {
    FILE *f = fopen(argv[i + 2], "rb");
    long sz;
    if (!f) {
      perror(argv[i + 2]);
      return 1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
      fprintf(stderr, "empty: %s\n", argv[i + 2]);
      fclose(f);
      return 1;
    }
    blobs[i] = (uint8_t *)malloc((size_t)sz);
    sizes[i] = (uint32_t)sz;
    if (!blobs[i] || fread(blobs[i], 1, (size_t)sz, f) != (size_t)sz) {
      fprintf(stderr, "read failed: %s\n", argv[i + 2]);
      fclose(f);
      return 1;
    }
    fclose(f);

    memset(hdr.files[i].name, 0, INITRD_NAME_MAX);
    strncpy(hdr.files[i].name, basename_of(argv[i + 2]), INITRD_NAME_MAX - 1);
    hdr.files[i].offset = offset;
    hdr.files[i].size = sizes[i];
    offset += sizes[i];
  }

  out = fopen(argv[1], "wb");
  if (!out) {
    perror(argv[1]);
    return 1;
  }
  if (fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
    fprintf(stderr, "write header failed\n");
    return 1;
  }
  for (i = 0; i < (int)hdr.count; i++) {
    if (fwrite(blobs[i], 1, sizes[i], out) != sizes[i]) {
      fprintf(stderr, "write blob failed\n");
      return 1;
    }
    free(blobs[i]);
  }
  fclose(out);
  printf("wrote %s (%u modules, %u bytes)\n", argv[1], hdr.count, offset);
  return 0;
}
