#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

extern void *ksym_lookup(const char *name);

typedef enum { PROC_DIR, PROC_MEMINFO, PROC_MODULES, PROC_MOUNTS,
               PROC_SELF, PROC_STATUS, PROC_VERSION } proc_kind_t;
typedef struct proc_node {
    const char *name;
    proc_kind_t kind;
    inode_t *inode;
} proc_node_t;

static inode_t *(*alloc_inode)(super_block_t *, uint32_t);
static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static size_t (*heap_used_fn)(void);
static size_t (*heap_free_fn)(void);
static process_t *(*process_current_fn)(void);
static super_block_t *g_sb;
static proc_node_t g_nodes[] = {
    { "meminfo", PROC_MEMINFO, NULL }, { "modules", PROC_MODULES, NULL },
    { "mounts", PROC_MOUNTS, NULL }, { "self", PROC_SELF, NULL },
    { "status", PROC_STATUS, NULL }, { "version", PROC_VERSION, NULL }
};

static size_t append_uint(char *buf, size_t at, uint64_t n)
{
    char tmp[24];
    size_t i = 0, j;
    do { tmp[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    for (j = 0; j < i; j++) buf[at + j] = tmp[i - j - 1];
    return at + i;
}

static size_t proc_content(proc_node_t *node, char *buf)
{
    size_t at = 0;
    process_t *p;
    if (node->kind == PROC_MEMINFO) {
        memcpy(buf, "HeapUsed: ", 10); at = 10;
        at = append_uint(buf, at, heap_used_fn ? heap_used_fn() : 0);
        memcpy(buf + at, "\nHeapFree: ", 11); at += 11;
        at = append_uint(buf, at, heap_free_fn ? heap_free_fn() : 0);
        buf[at++] = '\n';
    } else if (node->kind == PROC_MODULES) {
        memcpy(buf, "modules\n", 8); at = 8;
    } else if (node->kind == PROC_MOUNTS) {
        memcpy(buf, "rootfs /\n", 9); at = 9;
    } else if (node->kind == PROC_STATUS) {
        memcpy(buf, "Pid: ", 5); at = 5;
        p = process_current_fn ? process_current_fn() : NULL;
        at = append_uint(buf, at, p ? (uint64_t)p->pid : 0);
        buf[at++] = '\n';
    } else if (node->kind == PROC_VERSION) {
        memcpy(buf, "mykernel\n", 9); at = 9;
    }
    return at;
}

static ssize_t proc_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    proc_node_t *node = (proc_node_t *)file->f_inode->i_private;
    char content[96];
    size_t len;
    if (!node || node->kind == PROC_DIR || node->kind == PROC_SELF)
        return -EISDIR;
    len = proc_content(node, content);
    if (*pos < 0 || (size_t)*pos >= len)
        return 0;
    if (count > len - (size_t)*pos) count = len - (size_t)*pos;
    memcpy(buf, content + *pos, count);
    *pos += (off_t)count;
    return (ssize_t)count;
}

static dentry_t *proc_lookup(inode_t *dir, dentry_t *dentry)
{
    proc_node_t *parent = (proc_node_t *)dir->i_private;
    size_t i;
    if (parent && parent->kind == PROC_SELF && strcmp(dentry->d_name, "status") == 0)
        return alloc_dentry("status", dentry->d_parent, g_nodes[4].inode);
    if (parent && parent->kind != PROC_DIR)
        return NULL;
    for (i = 0; i < 6; i++)
        if (g_nodes[i].kind != PROC_STATUS && strcmp(g_nodes[i].name, dentry->d_name) == 0)
            return alloc_dentry(dentry->d_name, dentry->d_parent, g_nodes[i].inode);
    return NULL;
}

static int proc_readdir(file_t *file, void *dirent, size_t max)
{
    proc_node_t *node = (proc_node_t *)file->f_inode->i_private;
    vfs_dirent_t *out = dirent;
    size_t i, w = 0;
    if (!node || (node->kind != PROC_DIR && node->kind != PROC_SELF)) return -ENOTDIR;
    if (node->kind == PROC_SELF) {
        if (max) { strcpy(out[0].name, "status"); out[0].ino = g_nodes[4].inode->i_ino; out[0].type = S_IFREG; }
        return max ? 1 : 0;
    }
    for (i = 0; i < 6 && w < max; i++) {
        if (g_nodes[i].kind == PROC_STATUS) continue;
        strncpy(out[w].name, g_nodes[i].name, sizeof(out[w].name) - 1);
        out[w].ino = g_nodes[i].inode->i_ino;
        out[w].type = g_nodes[i].kind == PROC_SELF ? S_IFDIR : S_IFREG;
        w++;
    }
    return (int)w;
}

static const inode_operations_t proc_iops = { .lookup = proc_lookup };
static const file_operations_t proc_fops = { .read = proc_read, .readdir = proc_readdir };

static int procfs_mount(file_system_type_t *fs_type, int flags,
                        const char *dev_name, void *data, super_block_t **sb_out)
{
    inode_t *root;
    dentry_t *dentry;
    size_t i;
    (void)flags; (void)dev_name; (void)data;
    g_sb = kmalloc(sizeof(*g_sb));
    if (!g_sb) return -ENOMEM;
    memset(g_sb, 0, sizeof(*g_sb));
    g_sb->s_type = fs_type;
    root = alloc_inode(g_sb, S_IFDIR | 0555);
    if (!root) return -ENOMEM;
    root->i_op = &proc_iops; root->i_fop = &proc_fops;
    {
        static proc_node_t root_node = { "/", PROC_DIR, NULL };
        root_node.inode = root; root->i_private = &root_node;
    }
    for (i = 0; i < 6; i++) {
        g_nodes[i].inode = alloc_inode(g_sb, (g_nodes[i].kind == PROC_SELF ? S_IFDIR : S_IFREG) | 0444);
        if (!g_nodes[i].inode) return -ENOMEM;
        g_nodes[i].inode->i_op = &proc_iops; g_nodes[i].inode->i_fop = &proc_fops;
        g_nodes[i].inode->i_private = &g_nodes[i];
    }
    dentry = alloc_dentry("/", NULL, root);
    if (!dentry) return -ENOMEM;
    g_sb->s_root = dentry; *sb_out = g_sb;
    return 0;
}

static file_system_type_t g_fs = {
    .name = "procfs",
    .mount = procfs_mount,
};

static int procfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem || !api->alloc_inode || !api->alloc_dentry ||
        !api->mkdir || !api->mount)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    heap_used_fn = (size_t (*)(void))ksym_lookup("heap_used");
    heap_free_fn = (size_t (*)(void))ksym_lookup("heap_free");
    process_current_fn = (process_t *(*)(void))ksym_lookup("process_current");
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    (void)api->mkdir("/proc", 0555);
    if (api->mount("procfs", "/proc", "procfs", 0, NULL) < 0)
        return -1;
    vga_print("procfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "procfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 70;
    d.init = procfs_init;
    if (driver_register(&d) < 0) return -1;
    if (driver_load("procfs", NULL) < 0) return -1;
    return 0;
}
