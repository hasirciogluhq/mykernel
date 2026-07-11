#include <kernel/vfs.h>
#include <kernel/string.h>
#include <drivers/vga.h>

static vfs_node_t nodes[VFS_MAX_FILES];
static int        node_count;

/* Global open-file table (kernel-wide); process maps user fd -> index */
static vfs_file_t open_files[VFS_MAX_FILES * 2];

static ssize_t console_write(vfs_node_t *node, const void *buf, size_t count, off_t off)
{
    (void)node;
    (void)off;
    vga_write((const char *)buf, count);
    return (ssize_t)count;
}

static ssize_t console_read(vfs_node_t *node, void *buf, size_t count, off_t off)
{
    (void)node;
    (void)buf;
    (void)count;
    (void)off;
    return 0; /* no keyboard yet */
}

static vfs_node_t *find_node(const char *path)
{
    const char *name = path;
    if (name[0] == '/')
        name++;

    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].name, name) == 0)
            return &nodes[i];
    }
    return NULL;
}

static int alloc_open_slot(void)
{
    for (int i = 0; i < (int)(sizeof(open_files) / sizeof(open_files[0])); i++) {
        if (!open_files[i].used)
            return i;
    }
    return -1;
}

void vfs_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(open_files, 0, sizeof(open_files));
    node_count = 0;

    vfs_register_device("dev/console", console_read, console_write);

    static const char motd[] = "welcome to mykernel\n";
    static const char hello[] = "PING\nPONG\n";
    vfs_register_file("motd", motd, sizeof(motd) - 1);
    vfs_register_file("hello.txt", hello, sizeof(hello) - 1);
}

int vfs_register_file(const char *name, const void *data, size_t size)
{
    if (node_count >= VFS_MAX_FILES)
        return -1;

    vfs_node_t *n = &nodes[node_count++];
    strncpy(n->name, name, VFS_MAX_NAME - 1);
    n->type = VFS_TYPE_FILE;
    n->data = (uint8_t *)data; /* assume rodata / static lifetime */
    n->size = size;
    n->capacity = size;
    n->dev_read = NULL;
    n->dev_write = NULL;
    return 0;
}

int vfs_register_device(const char *name,
                        ssize_t (*read_fn)(vfs_node_t *, void *, size_t, off_t),
                        ssize_t (*write_fn)(vfs_node_t *, const void *, size_t, off_t))
{
    if (node_count >= VFS_MAX_FILES)
        return -1;

    vfs_node_t *n = &nodes[node_count++];
    strncpy(n->name, name, VFS_MAX_NAME - 1);
    n->type = VFS_TYPE_DEVICE;
    n->data = NULL;
    n->size = 0;
    n->capacity = 0;
    n->dev_read = read_fn;
    n->dev_write = write_fn;
    return 0;
}

int vfs_open(const char *path, int flags)
{
    vfs_node_t *node = find_node(path);
    if (!node)
        return -1;

    int slot = alloc_open_slot();
    if (slot < 0)
        return -1;

    open_files[slot].node = node;
    open_files[slot].offset = 0;
    open_files[slot].flags = flags;
    open_files[slot].used = 1;
    return slot;
}

ssize_t vfs_read(int fd, void *buf, size_t count)
{
    if (fd < 0 || !open_files[fd].used)
        return -1;

    vfs_file_t *f = &open_files[fd];
    if (f->flags == O_WRONLY)
        return -1;

    vfs_node_t *n = f->node;
    if (n->type == VFS_TYPE_DEVICE && n->dev_read)
        return n->dev_read(n, buf, count, f->offset);

    if (!n->data)
        return 0;

    if ((size_t)f->offset >= n->size)
        return 0;

    size_t avail = n->size - (size_t)f->offset;
    if (count > avail)
        count = avail;

    memcpy(buf, n->data + f->offset, count);
    f->offset += (off_t)count;
    return (ssize_t)count;
}

ssize_t vfs_write(int fd, const void *buf, size_t count)
{
    if (fd < 0 || !open_files[fd].used)
        return -1;

    vfs_file_t *f = &open_files[fd];
    if (f->flags == O_RDONLY)
        return -1;

    vfs_node_t *n = f->node;
    if (n->type == VFS_TYPE_DEVICE && n->dev_write)
        return n->dev_write(n, buf, count, f->offset);

    return -1; /* regular files are read-only for now */
}

int vfs_close(int fd)
{
    if (fd < 0 || !open_files[fd].used)
        return -1;
    memset(&open_files[fd], 0, sizeof(open_files[fd]));
    return 0;
}
