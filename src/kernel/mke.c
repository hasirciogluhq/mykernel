#include <kernel/mke.h>
#include <kernel/argv.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/initrd.h>
#include <kernel/initrd_store.h>
#include <kernel/vfs.h>
#include <kernel/mkdx_api.h>
#include <kernel/syscall.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <multiboot.h>

static int name_ends_with_mke(const char *name)
{
    size_t n;
    if (!name)
        return 0;
    n = strlen(name);
    if (n < 4)
        return 0;
    return strcmp(name + n - 4, ".mke") == 0;
}

static int mke_name_is(const char *name, const char *stem)
{
    size_t sn;
    size_t nn;

    if (!name || !stem)
        return 0;
    sn = strlen(stem);
    nn = strlen(name);
    if (strcmp(name, stem) == 0)
        return 1;
    if (nn == sn + 4 && strncmp(name, stem, sn) == 0 &&
        strcmp(name + sn, ".mke") == 0)
        return 1;
    return 0;
}

/* Boot: only os-ui; skip imgui-demo and on-demand apps (terminal, settings, …). */
static const char *mke_path_basename(const char *path)
{
    const char *base = path;

    if (!path)
        return "";
    while (*path) {
        if (*path == '/')
            base = path + 1;
        path++;
    }
    return base;
}

static int mke_boot_should_spawn(const char *name)
{
    const char *base = mke_path_basename(name);

    if (!base || !base[0])
        return 0;
    if (mke_name_is(base, "imgui-demo"))
        return 0;
    return mke_name_is(base, "os-ui");
}

static void mke_attach_console(pid_t pid, const char *name, uint32_t spawn_flags)
{
    const mkdx_api_t *api = mkdx_api_get();
    int visible;

    if (!api || !api->console_alloc || pid <= 0)
        return;

    visible = (spawn_flags & SPAWN_CONSOLE_VISIBLE) ? 1 : 0;
    (void)api->console_alloc((int)pid, name, visible);
}

static const uint8_t *mke_initrd_lookup(const char *path, size_t *size_out)
{
    const char *name = mke_path_basename(path);
    size_t size = 0;
    const initrd_header_t *hdr;
    size_t table_bytes;
    uint32_t i;

    if (size_out)
        *size_out = 0;
    if (!name[0])
        return NULL;

    hdr = (const initrd_header_t *)initrd_store_get(&size);
    if (!hdr || size < sizeof(uint32_t) * 2 || hdr->magic != INITRD_MAGIC ||
        hdr->count == 0 || hdr->count > INITRD_MAX_FILES)
        return NULL;

    table_bytes = sizeof(uint32_t) * 2 + (size_t)hdr->count * sizeof(initrd_file_t);
    if (size < table_bytes)
        return NULL;

    for (i = 0; i < hdr->count; i++) {
        const initrd_file_t *f = &hdr->files[i];

        if (strcmp(f->name, name) != 0)
            continue;
        if (f->size == 0 || f->offset + f->size > size)
            return NULL;
        if (size_out)
            *size_out = f->size;
        return (const uint8_t *)hdr + f->offset;
    }

    return NULL;
}

static int mke_validate_header(const mke_header_t *hdr, size_t total_size)
{
    if (!hdr || total_size < sizeof(mke_header_t)) {
        klog("[mke] spawn: bad blob\n");
        return -1;
    }
    if (hdr->magic != MKE_MAGIC || hdr->version != MKE_VERSION) {
        klog("[mke] spawn: bad magic/version\n");
        return -1;
    }
    if (hdr->header_size != sizeof(mke_header_t)) {
        klog("[mke] spawn: bad header_size\n");
        return -1;
    }
    if (hdr->load_addr < MKE_LOAD_MIN || hdr->load_addr > MKE_LOAD_MAX) {
        klog("[mke] spawn: load_addr out of range ");
        serial_print_hex(hdr->load_addr);
        klog("\n");
        return -1;
    }
    if (hdr->image_size == 0) {
        klog("[mke] spawn: empty image\n");
        return -1;
    }
    if ((size_t)hdr->header_size + (size_t)hdr->image_size > total_size) {
        klog("[mke] spawn: image exceeds blob\n");
        return -1;
    }
    if (hdr->entry_off >= hdr->image_size + hdr->bss_size) {
        klog("[mke] spawn: bad entry_off\n");
        return -1;
    }
    if (hdr->load_addr + hdr->image_size + hdr->bss_size < hdr->load_addr) {
        klog("[mke] spawn: load region wrap\n");
        return -1;
    }
    if (hdr->load_addr + hdr->image_size + hdr->bss_size > MKE_LOAD_MAX + 0x00800000u) {
        klog("[mke] spawn: load region too large\n");
        return -1;
    }
    return 0;
}

static void mke_zero_bss(const mke_header_t *hdr)
{
    if (!hdr || hdr->bss_size == 0)
        return;
    memset((void *)(uintptr_t)(hdr->load_addr + hdr->image_size), 0, hdr->bss_size);
}

/*
 * Single address space: reloading an .mke at a fixed load_addr overwrites any
 * still-running instance. Kill those first so we do not corrupt live EIP/data
 * or leave orphan windows / PROC slots (dock spam → process_create_user FAILED).
 */
static void mke_kill_load_overlap(const mke_header_t *hdr)
{
    process_t **table;
    uint32_t lo, hi;
    int i;

    if (!hdr)
        return;
    lo = hdr->load_addr;
    hi = hdr->load_addr + hdr->image_size + hdr->bss_size;
    if (hi < lo)
        return;

    table = process_table();
    if (!table)
        return;

    for (i = 0; i < PROC_MAX; i++) {
        process_t *p = table[i];
        uint32_t entry;

        if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
            continue;
        if (!p->is_user || !p->user_entry)
            continue;
        entry = (uint32_t)(uintptr_t)p->user_entry;
        if (entry < lo || entry >= hi)
            continue;
        (void)process_kill(p->pid);
    }
}

static int mke_spawn_header(const mke_header_t *hdr, uint32_t spawn_flags,
                            const char *const *argv, int argc)
{
    void (*entry)(void);
    pid_t pid;

    entry = (void (*)(void))(uintptr_t)(hdr->load_addr + hdr->entry_off);
    pid = process_create_user(hdr->name[0] ? hdr->name : "mke", entry);
    if (pid < 0) {
        klog("[mke] process_create_user FAILED\n");
        vga_print("mke: process_create_user failed\n");
        return -1;
    }

    if (argv && argc > 0) {
        process_t *child = process_get(pid);
        if (child && argv_proc_set(child, argv, argc) < 0) {
            (void)process_kill(pid);
            return -EINVAL;
        }
    }

    klog("[mke] spawned ");
    klog(hdr->name);
    klog(" pid=");
    serial_print_uint((uint32_t)pid);
    klog(" entry=");
    serial_print_hex((uint32_t)(uintptr_t)entry);
    klog("\n");

    mke_attach_console(pid, hdr->name[0] ? hdr->name : "mke", spawn_flags);
    return pid;
}

int mke_spawn_flags(const void *blob, size_t size, uint32_t spawn_flags,
                    const char *const *argv, int argc)
{
    const mke_header_t *hdr;
    const uint8_t *img;
    uint8_t *dst;

    if (mke_validate_header((const mke_header_t *)blob, size) < 0)
        return -1;

    hdr = (const mke_header_t *)blob;

    klog("[mke] loading ");
    klog(hdr->name[0] ? hdr->name : "?");
    klog(" @ ");
    serial_print_hex(hdr->load_addr);
    klog(" img=");
    serial_print_uint(hdr->image_size);
    klog(" bss=");
    serial_print_uint(hdr->bss_size);
    klog("\n");

    mke_kill_load_overlap(hdr);

    img = (const uint8_t *)blob + hdr->header_size;
    dst = (uint8_t *)(uintptr_t)hdr->load_addr;
    memcpy(dst, img, hdr->image_size);
    mke_zero_bss(hdr);

    return mke_spawn_header(hdr, spawn_flags, argv, argc);
}

int mke_spawn(const void *blob, size_t size)
{
    return mke_spawn_flags(blob, size, SPAWN_CONSOLE_HIDDEN, NULL, 0);
}

int mke_spawn_path_flags(const char *path, uint32_t spawn_flags,
                         const char *const *argv, int argc)
{
    const uint8_t *initrd_blob;
    size_t initrd_size = 0;
    mke_header_t hdr;
    int fd;
    off_t end;
    ssize_t n;
    size_t loaded;
    uint8_t *dst;

    if (!path || !path[0])
        return -EINVAL;

    initrd_blob = mke_initrd_lookup(path, &initrd_size);
    if (initrd_blob)
        return mke_spawn_flags(initrd_blob, initrd_size, spawn_flags, argv, argc);

    fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
        return fd;

    end = vfs_lseek(fd, 0, SEEK_END);
    if (end <= 0) {
        (void)vfs_close(fd);
        return end < 0 ? (int)end : -ENOEXEC;
    }
    if (vfs_lseek(fd, 0, SEEK_SET) < 0) {
        (void)vfs_close(fd);
        return -EIO;
    }

    n = vfs_read(fd, &hdr, sizeof(hdr));
    if (n < 0) {
        (void)vfs_close(fd);
        return (int)n;
    }
    if ((size_t)n != sizeof(hdr)) {
        (void)vfs_close(fd);
        return -ENOEXEC;
    }
    if (mke_validate_header(&hdr, (size_t)end) < 0) {
        (void)vfs_close(fd);
        return -ENOEXEC;
    }

    klog("[mke] loading ");
    klog(hdr.name[0] ? hdr.name : "?");
    klog(" @ ");
    serial_print_hex(hdr.load_addr);
    klog(" img=");
    serial_print_uint(hdr.image_size);
    klog(" bss=");
    serial_print_uint(hdr.bss_size);
    klog("\n");

    mke_kill_load_overlap(&hdr);

    dst = (uint8_t *)(uintptr_t)hdr.load_addr;
    if (vfs_lseek(fd, (off_t)hdr.header_size, SEEK_SET) < 0) {
        (void)vfs_close(fd);
        return -EIO;
    }

    loaded = 0;
    while (loaded < hdr.image_size) {
        size_t chunk = hdr.image_size - loaded;
        n = vfs_read(fd, dst + loaded, chunk);
        if (n < 0) {
            (void)vfs_close(fd);
            return (int)n;
        }
        if (n == 0) {
            (void)vfs_close(fd);
            return -EIO;
        }
        loaded += (size_t)n;
    }

    (void)vfs_close(fd);
    mke_zero_bss(&hdr);
    return mke_spawn_header(&hdr, spawn_flags, argv, argc);
}

int mke_spawn_path(const char *path)
{
    return mke_spawn_path_flags(path, SPAWN_CONSOLE_HIDDEN, NULL, 0);
}

int mke_spawn_from_initrd(const void *data, size_t size)
{
    const initrd_header_t *hdr;
    uint32_t i;
    int spawned = 0;
    size_t table_bytes;

    if (!data || size < sizeof(uint32_t) * 2)
        return -1;

    hdr = (const initrd_header_t *)data;
    if (hdr->magic != INITRD_MAGIC || hdr->count == 0 ||
        hdr->count > INITRD_MAX_FILES)
        return -1;

    table_bytes = sizeof(uint32_t) * 2 + (size_t)hdr->count * sizeof(initrd_file_t);
    if (size < table_bytes)
        return -1;

    for (i = 0; i < hdr->count; i++) {
        const initrd_file_t *f = &hdr->files[i];
        const uint8_t *blob;
        const mke_header_t *mh;

        if (f->size == 0 || f->offset + f->size > size)
            return -1;
        blob = (const uint8_t *)data + f->offset;
        if (f->size < sizeof(mke_header_t))
            continue;
        mh = (const mke_header_t *)blob;
        if (mh->magic != MKE_MAGIC && !name_ends_with_mke(f->name))
            continue;
        if (!mke_boot_should_spawn(f->name))
            continue;
        klog("[mke] initrd file ");
        klog(f->name);
        klog(" size=");
        serial_print_uint(f->size);
        klog("\n");
        /* Soft-fail one app so others (and the scheduler) can still start. */
        if (mke_spawn(blob, f->size) < 0) {
            klog("[mke] spawn failed, skipping ");
            klog(f->name);
            klog("\n");
            continue;
        }
        spawned++;
    }

    klog_uint("[mke] initrd spawned count=", (uint32_t)spawned);
    return 0;
}

int mke_spawn_from_mbi(multiboot_info_t *mbi)
{
    multiboot_mod_list_t *mods;
    uint32_t i;
    int any = 0;

    if (!mbi || !(mbi->flags & MULTIBOOT_INFO_MODS) || mbi->mods_count == 0)
        return -1;

    mods = (multiboot_mod_list_t *)(uintptr_t)mbi->mods_addr;
    for (i = 0; i < mbi->mods_count; i++) {
        const void *start = (const void *)(uintptr_t)mods[i].mod_start;
        size_t sz = (size_t)(mods[i].mod_end - mods[i].mod_start);
        const initrd_header_t *hdr;
        const mke_header_t *mh;

        if (sz < 4)
            continue;

        hdr = (const initrd_header_t *)start;
        if (hdr->magic == INITRD_MAGIC) {
            if (mke_spawn_from_initrd(start, sz) == 0)
                any = 1;
            continue;
        }

        mh = (const mke_header_t *)start;
        if (mh->magic == MKE_MAGIC) {
            const char *boot_name = mh->name[0] ? mh->name : "mke";
            if (!mke_boot_should_spawn(boot_name))
                continue;
            if (mke_spawn(start, sz) < 0)
                return -1;
            any = 1;
        }
    }

    return any ? 0 : -1;
}
