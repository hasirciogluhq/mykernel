#ifndef MYKERNEL_DRIVERS_VFS_FS_H
#define MYKERNEL_DRIVERS_VFS_FS_H

#include <kernel/types.h>
#include <kernel/block_api.h>

#define VFS_NAME_MAX    255
#define VFS_PATH_MAX    256
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

struct inode;
struct dentry;
struct file;
struct super_block;
struct vfsmount;
struct file_system_type;

typedef struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);
    int (*create)(struct inode *dir, struct dentry *dentry, int mode);
    int (*link)(struct dentry *old, struct inode *dir, struct dentry *newd);
    int (*unlink)(struct inode *dir, struct dentry *dentry);
    int (*symlink)(struct inode *dir, struct dentry *dentry, const char *target);
    int (*mkdir)(struct inode *dir, struct dentry *dentry, int mode);
    int (*rmdir)(struct inode *dir, struct dentry *dentry);
    int (*mknod)(struct inode *dir, struct dentry *dentry, int mode, uint32_t dev);
    int (*rename)(struct inode *old_dir, struct dentry *old_d,
                  struct inode *new_dir, struct dentry *new_d);
    int (*setattr)(struct dentry *dentry, void *attr);
    int (*getattr)(struct dentry *dentry, void *statbuf);
    int (*listxattr)(struct dentry *dentry, char *list, size_t size);
    int (*permission)(struct inode *inode, int mask);
    const char *(*get_link)(struct dentry *dentry, struct inode *inode);
} inode_operations_t;

typedef struct file_operations {
    ssize_t (*read)(struct file *file, void *buf, size_t count, off_t *pos);
    ssize_t (*write)(struct file *file, const void *buf, size_t count, off_t *pos);
    off_t   (*llseek)(struct file *file, off_t off, int whence);
    int     (*mmap)(struct file *file, void *vma);
    int     (*ioctl)(struct file *file, unsigned long cmd, void *arg);
    int     (*fsync)(struct file *file);
    int     (*flush)(struct file *file);
    int     (*poll)(struct file *file, void *wait);
    int     (*lock)(struct file *file, int cmd, void *fl);
    int     (*fallocate)(struct file *file, int mode, off_t off, off_t len);
    int     (*open)(struct inode *inode, struct file *file);
    int     (*release)(struct inode *inode, struct file *file);
    int     (*readdir)(struct file *file, void *dirent, size_t max);
    /* Async path: return 0 if queued, complete via callback later. */
    int     (*aio_read)(struct file *file, void *buf, size_t count, off_t pos,
                        void (*done)(void *ctx, ssize_t n), void *ctx);
    int     (*aio_write)(struct file *file, const void *buf, size_t count, off_t pos,
                         void (*done)(void *ctx, ssize_t n), void *ctx);
} file_operations_t;

typedef struct dentry_operations {
    uint32_t (*d_hash)(const struct dentry *parent, const char *name, size_t len);
    int (*d_compare)(const struct dentry *parent, const char *a, size_t alen,
                     const char *b, size_t blen);
    int (*d_delete)(const struct dentry *dentry);
    int (*d_revalidate)(struct dentry *dentry);
} dentry_operations_t;

typedef struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *inode);
    void (*put_super)(struct super_block *sb);
    int  (*statfs)(struct super_block *sb, void *buf);
    int  (*remount_fs)(struct super_block *sb, int *flags, char *data);
    int  (*sync_fs)(struct super_block *sb, int wait);
    int  (*show_options)(struct super_block *sb, char *buf, size_t len);
} super_operations_t;

typedef struct inode {
    uint32_t i_ino;
    uint32_t i_mode;
    uint32_t i_uid;
    uint32_t i_gid;
    uint32_t i_nlink;
    uint64_t i_size;
    uint32_t i_atime;
    uint32_t i_mtime;
    uint32_t i_ctime;
    const inode_operations_t *i_op;
    const file_operations_t  *i_fop;
    struct super_block       *i_sb;
    void                     *i_private;
    int                       i_ref;
} inode_t;

typedef struct dentry {
    char               d_name[64];
    struct dentry     *d_parent;
    inode_t           *d_inode;
    struct super_block *d_sb;
    const dentry_operations_t *d_op;
    void              *d_private;
    int                d_ref;
} dentry_t;

typedef struct file {
    dentry_t                 *f_dentry;
    inode_t                  *f_inode;
    const file_operations_t  *f_op;
    off_t                     f_pos;
    int                       f_flags;
    void                     *private_data;
    int                       used;
} file_t;

typedef struct super_block {
    struct file_system_type  *s_type;
    dentry_t                 *s_root;
    block_device_t           *s_bdev;
    const super_operations_t *s_op;
    unsigned long             s_flags;
    char                      s_id[32];
    void                     *s_fs_info;
    int                       s_ref;
} super_block_t;

typedef struct vfsmount {
    char           mnt_devname[64];
    char           mnt_path[VFS_PATH_MAX];
    super_block_t *mnt_sb;
    dentry_t      *mnt_root;
    dentry_t      *mnt_mountpoint;
    struct vfsmount *mnt_parent;
    unsigned long  mnt_flags;
    int            used;
} vfsmount_t;

typedef struct file_system_type {
    const char *name;
    int         flags;
    int (*mount)(struct file_system_type *fs_type, int flags,
                 const char *dev_name, void *data, super_block_t **sb_out);
    void (*kill_sb)(super_block_t *sb);
    struct file_system_type *next;
} file_system_type_t;

typedef struct vfs_stat {
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_nlink;
    uint64_t st_size;
    uint32_t st_ino;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
} vfs_stat_t;

typedef struct vfs_dirent {
    uint32_t ino;
    uint32_t type; /* S_IF* */
    char     name[64];
} vfs_dirent_t;

#endif
