#ifndef MYKERNEL_KERNEL_BLOCK_API_H
#define MYKERNEL_KERNEL_BLOCK_API_H

#include <kernel/types.h>

struct block_device;
struct bio;
struct gendisk;

typedef void (*bio_end_io_t)(struct bio *bio);

typedef struct bio {
    struct block_device *bdev;
    uint64_t             sector; /* 512-byte sectors */
    void                *data;
    size_t               len;    /* bytes, multiple of 512 preferred */
    int                  write;  /* 0=read 1=write */
    volatile int         done;
    int                  error;
    bio_end_io_t         end_io;
    void                *private_data;
} bio_t;

typedef struct block_ops {
    int  (*submit_bio)(bio_t *bio); /* async; may complete inline */
    int  (*poll)(struct block_device *bdev);
    int  (*ioctl)(struct block_device *bdev, unsigned long cmd, void *arg);
} block_ops_t;

typedef struct block_device {
    char          name[32];
    uint64_t      capacity; /* sectors */
    uint32_t      sector_size;
    const block_ops_t *ops;
    void         *private_data;
    struct gendisk *disk;
    int           major;
    int           minor;
    int           partition; /* 0 = whole disk */
} block_device_t;

typedef struct partition_ops {
    const char *name;
    /* Scan parent disk; call add_partition for each found entry. */
    int (*probe)(block_device_t *disk);
} partition_ops_t;

typedef struct block_api {
    int  (*register_blkdev)(int major, const char *name);
    int  (*add_disk)(block_device_t *bdev);
    int  (*del_disk)(block_device_t *bdev);
    int  (*add_partition)(block_device_t *parent, int partno,
                          uint64_t start_sect, uint64_t nsect, const char *name);
    block_device_t *(*lookup)(const char *name);
    int  (*submit_bio)(bio_t *bio);
    int  (*submit_bio_sync)(bio_t *bio);
    int  (*read)(block_device_t *bdev, uint64_t sector, void *buf, size_t nsect);
    int  (*write)(block_device_t *bdev, uint64_t sector, const void *buf, size_t nsect);
    int  (*register_partition_driver)(const partition_ops_t *ops);
    int  (*scan_partitions)(block_device_t *disk);
    int  (*poll)(void);
    size_t (*disk_count)(void);
    block_device_t *(*disk_at)(size_t index);
} block_api_t;

void               block_api_register(const block_api_t *api);
const block_api_t *block_api_get(void);

#endif
