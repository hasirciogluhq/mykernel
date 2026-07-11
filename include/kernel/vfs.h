#ifndef MYKERNEL_VFS_H
#define MYKERNEL_VFS_H

#include <kernel/types.h>

#define VFS_MAX_FILES   16
#define VFS_MAX_NAME    32
#define VFS_MAX_FD      16
#define VFS_PATH_MAX    64

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

typedef enum {
    VFS_TYPE_FILE = 0,
    VFS_TYPE_DEVICE
} vfs_type_t;

typedef struct vfs_node {
    char       name[VFS_MAX_NAME];
    vfs_type_t type;
    uint8_t   *data;
    size_t     size;
    size_t     capacity;
    /* device ops (optional) */
    ssize_t  (*dev_read)(struct vfs_node *node, void *buf, size_t count, off_t off);
    ssize_t  (*dev_write)(struct vfs_node *node, const void *buf, size_t count, off_t off);
} vfs_node_t;

typedef struct {
    vfs_node_t *node;
    off_t       offset;
    int         flags;
    int         used;
} vfs_file_t;

void vfs_init(void);
int  vfs_register_file(const char *name, const void *data, size_t size);
int  vfs_register_device(const char *name,
                        ssize_t (*read)(vfs_node_t *, void *, size_t, off_t),
                        ssize_t (*write)(vfs_node_t *, const void *, size_t, off_t));

int     vfs_open(const char *path, int flags);
ssize_t vfs_read(int fd, void *buf, size_t count);
ssize_t vfs_write(int fd, const void *buf, size_t count);
int     vfs_close(int fd);

#endif
