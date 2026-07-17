#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/pci.h>

#define PCI_VENDOR_VIRTIO     0x1AF4
#define PCI_DEVICE_BLK_MODERN 0x1042
#define PCI_DEVICE_BLK_TRANS  0x1001

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vdesc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) vavail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vused_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vused_elem_t ring[];
} __attribute__((packed)) vused_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_t;

typedef struct pending {
    bio_t *bio;
    uint8_t status;
    virtio_blk_req_t hdr;
    int used;
} pending_t;

typedef struct {
    pci_device_t pci;
    volatile uint8_t *common;
    volatile uint8_t *notify;
    volatile uint8_t *isr;
    volatile uint8_t *devcfg;
    uint32_t notify_mult;
    uint16_t qnotify_off;
    uint16_t qsize;
    vdesc_t *desc;
    vavail_t *avail;
    vused_t *used;
    void *qmem;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;
    pending_t pend[16];
    block_device_t bdev;
    uint64_t capacity;
} vblk_t;

static inline void mmio_w8(volatile uint8_t *p, uint8_t v) { *p = v; }
static inline uint8_t mmio_r8(volatile uint8_t *p) { return *p; }
static inline void mmio_w16(volatile uint8_t *p, uint16_t v) { *(volatile uint16_t *)p = v; }
static inline uint16_t mmio_r16(volatile uint8_t *p) { return *(volatile uint16_t *)p; }
static inline void mmio_w32(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline uint32_t mmio_r32(volatile uint8_t *p) { return *(volatile uint32_t *)p; }

static volatile uint8_t *map_bar(const pci_device_t *pci, uint8_t bar, uint32_t off)
{
    uint32_t base = pci_bar_phys(pci, (int)bar);
    if (!base)
        return NULL;
    return (volatile uint8_t *)(uintptr_t)(base + off);
}

static int parse_caps(vblk_t *vd)
{
    uint16_t status = pci_config_read16(vd->pci.bus, vd->pci.slot, vd->pci.func, 0x06);
    uint8_t cap;
    if (!(status & 0x10))
        return -1;
    cap = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, 0x34);
    while (cap) {
        uint8_t id = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, cap);
        if (id == 0x09) {
            uint8_t cfg = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 3));
            uint8_t bar = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 4));
            uint32_t offset =
                (uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 8)) |
                ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 9)) << 8) |
                ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 10)) << 16) |
                ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 11)) << 24);
            volatile uint8_t *ptr = map_bar(&vd->pci, bar, offset);
            if (!ptr)
                return -1;
            if (cfg == VIRTIO_PCI_CAP_COMMON_CFG)
                vd->common = ptr;
            else if (cfg == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                vd->notify = ptr;
                vd->notify_mult =
                    (uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 16)) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 17)) << 8) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 18)) << 16) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 19)) << 24);
            } else if (cfg == VIRTIO_PCI_CAP_ISR_CFG)
                vd->isr = ptr;
            else if (cfg == VIRTIO_PCI_CAP_DEVICE_CFG)
                vd->devcfg = ptr;
        }
        cap = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 1));
    }
    return (vd->common && vd->notify && vd->devcfg) ? 0 : -1;
}

static int setup_queue(vblk_t *vd)
{
    uint16_t qsz, i;
    size_t desc_sz, avail_sz, used_sz, total;
    uint8_t *mem;

    mmio_w16(vd->common + 22, 0); /* queue_select */
    qsz = mmio_r16(vd->common + 24);
    if (qsz == 0)
        return -1;
    if (qsz > 64)
        qsz = 64;
    vd->qsize = qsz;
    vd->qnotify_off = mmio_r16(vd->common + 30);

    desc_sz = sizeof(vdesc_t) * qsz;
    avail_sz = sizeof(vavail_t) + sizeof(uint16_t) * qsz;
    used_sz = sizeof(vused_t) + sizeof(vused_elem_t) * qsz;
    total = desc_sz + avail_sz + 4096 + used_sz;
    mem = (uint8_t *)kmalloc_aligned(total, 4096);
    if (!mem)
        return -1;
    memset(mem, 0, total);
    vd->qmem = mem;
    vd->desc = (vdesc_t *)mem;
    vd->avail = (vavail_t *)(mem + desc_sz);
    vd->used = (vused_t *)(mem + ((desc_sz + avail_sz + 4095) & ~4095u));
    vd->free_head = 0;
    vd->num_free = qsz;
    for (i = 0; i < qsz - 1; i++)
        vd->desc[i].next = i + 1;
    vd->desc[qsz - 1].next = 0xFFFF;

    mmio_w16(vd->common + 24, qsz);
    mmio_w32(vd->common + 32, (uint32_t)(uintptr_t)vd->desc);
    mmio_w32(vd->common + 36, 0);
    mmio_w32(vd->common + 40, (uint32_t)(uintptr_t)vd->avail);
    mmio_w32(vd->common + 44, 0);
    mmio_w32(vd->common + 48, (uint32_t)(uintptr_t)vd->used);
    mmio_w32(vd->common + 52, 0);
    mmio_w16(vd->common + 28, 1); /* queue_enable */
    return 0;
}

static int alloc_desc(vblk_t *vd)
{
    uint16_t i;
    if (!vd->num_free)
        return -1;
    i = vd->free_head;
    vd->free_head = vd->desc[i].next;
    vd->num_free--;
    return (int)i;
}

static void free_desc_chain(vblk_t *vd, uint16_t head)
{
    uint16_t i = head;
    for (;;) {
        uint16_t next = vd->desc[i].next;
        int last = !(vd->desc[i].flags & VRING_DESC_F_NEXT);
        vd->desc[i].next = vd->free_head;
        vd->free_head = i;
        vd->num_free++;
        if (last)
            break;
        i = next;
    }
}

static int vblk_poll(block_device_t *bdev)
{
    vblk_t *vd = (vblk_t *)bdev->private_data;
    int n = 0;
    if (!vd)
        return 0;
    while (vd->last_used != vd->used->idx) {
        vused_elem_t *ue = &vd->used->ring[vd->last_used % vd->qsize];
        uint16_t id = (uint16_t)ue->id;
        pending_t *p = NULL;
        int i;
        for (i = 0; i < 16; i++) {
            if (vd->pend[i].used && (uint16_t)(uintptr_t)vd->pend[i].bio == id) {
                /* id is desc head; match by scanning */
            }
        }
        /* Find pending by desc head stored in ring id */
        for (i = 0; i < 16; i++) {
            if (vd->pend[i].used) {
                p = &vd->pend[i];
                /* We store desc head in unused: use status addr trick — store head in bio private */
                break;
            }
        }
        /* Simpler: pend slots keyed by index stored in hdr.reserved */
        for (i = 0; i < 16; i++) {
            if (vd->pend[i].used && vd->pend[i].hdr.reserved == id) {
                p = &vd->pend[i];
                break;
            }
        }
        if (p && p->bio) {
            int err = (p->status == 0) ? 0 : -EIO;
            p->bio->error = err;
            p->bio->done = 1;
            if (p->bio->end_io)
                p->bio->end_io(p->bio);
            p->used = 0;
            n++;
        }
        free_desc_chain(vd, id);
        vd->last_used++;
    }
    if (vd->isr)
        (void)mmio_r8(vd->isr);
    return n;
}

static int vblk_submit(bio_t *bio)
{
    vblk_t *vd;
    int h0, h1, h2, slot, i;
    pending_t *p = NULL;
    uint16_t flags;

    if (!bio || !bio->bdev)
        return -EINVAL;
    vd = (vblk_t *)bio->bdev->private_data;
    if (!vd)
        return -EIO;

    for (i = 0; i < 16; i++) {
        if (!vd->pend[i].used) {
            p = &vd->pend[i];
            slot = i;
            break;
        }
    }
    if (!p)
        return -EAGAIN;

    h0 = alloc_desc(vd);
    h1 = alloc_desc(vd);
    h2 = alloc_desc(vd);
    if (h0 < 0 || h1 < 0 || h2 < 0)
        return -EAGAIN;

    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->bio = bio;
    p->hdr.type = bio->write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    p->hdr.sector = bio->sector;
    p->hdr.reserved = (uint32_t)h0; /* store head for completion match */
    p->status = 0xFF;

    vd->desc[h0].addr = (uint64_t)(uintptr_t)&p->hdr;
    vd->desc[h0].len = sizeof(p->hdr);
    vd->desc[h0].flags = VRING_DESC_F_NEXT;
    vd->desc[h0].next = (uint16_t)h1;

    flags = bio->write ? VRING_DESC_F_NEXT : (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE);
    vd->desc[h1].addr = (uint64_t)(uintptr_t)bio->data;
    vd->desc[h1].len = (uint32_t)bio->len;
    vd->desc[h1].flags = flags;
    vd->desc[h1].next = (uint16_t)h2;

    vd->desc[h2].addr = (uint64_t)(uintptr_t)&p->status;
    vd->desc[h2].len = 1;
    vd->desc[h2].flags = VRING_DESC_F_WRITE;
    vd->desc[h2].next = 0;

    vd->avail->ring[vd->avail->idx % vd->qsize] = (uint16_t)h0;
    __asm__ volatile("" ::: "memory");
    vd->avail->idx++;
    __asm__ volatile("" ::: "memory");
    mmio_w16(vd->notify + vd->qnotify_off * vd->notify_mult, 0);

    (void)slot;
    /* Try immediate completion */
    vblk_poll(&vd->bdev);
    return 0;
}

static const block_ops_t vblk_ops = {
    .submit_bio = vblk_submit,
    .poll = vblk_poll,
};

static int probe_pci(pci_device_t *out)
{
    if (pci_find(PCI_VENDOR_VIRTIO, PCI_DEVICE_BLK_MODERN, out) == 0)
        return 0;
    if (pci_find(PCI_VENDOR_VIRTIO, PCI_DEVICE_BLK_TRANS, out) == 0)
        return 0;
    return -1;
}

static int vblk_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    vblk_t *vd;
    pci_device_t pci;
    uint64_t cap;
    (void)drv;
    (void)ctx;

    if (!api)
        return -1;
    if (probe_pci(&pci) < 0) {
        vga_print("virtio_blk: no device\n");
        return 0; /* soft-fail */
    }

    vd = (vblk_t *)kmalloc(sizeof(*vd));
    if (!vd)
        return -1;
    memset(vd, 0, sizeof(*vd));
    vd->pci = pci;
    pci_enable_bus_master(&pci);
    if (parse_caps(vd) < 0)
        return 0;

    mmio_w8(vd->common + 20, 0);
    mmio_w8(vd->common + 20, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_w8(vd->common + 20, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    /* features: accept none beyond defaults */
    mmio_w32(vd->common + 8, 0);
    mmio_w32(vd->common + 4, 1);
    mmio_w32(vd->common + 8, 0);
    mmio_w8(vd->common + 20,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_r8(vd->common + 20) & VIRTIO_STATUS_FEATURES_OK))
        return 0;

    if (setup_queue(vd) < 0)
        return 0;

    cap = *(volatile uint64_t *)vd->devcfg;
    vd->capacity = cap;
    mmio_w8(vd->common + 20,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    memset(&vd->bdev, 0, sizeof(vd->bdev));
    strcpy(vd->bdev.name, "vda");
    vd->bdev.capacity = cap;
    vd->bdev.sector_size = 512;
    vd->bdev.ops = &vblk_ops;
    vd->bdev.private_data = vd;
    vd->bdev.major = 8;
    vd->bdev.minor = 0;

    if (api->add_disk(&vd->bdev) < 0)
        return 0;
    if (api->scan_partitions)
        api->scan_partitions(&vd->bdev);
    vga_print("virtio_blk: vda ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "virtio_blk", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 52;
    d.init = vblk_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("virtio_blk", NULL) < 0)
        return 0;
    return 0;
}
