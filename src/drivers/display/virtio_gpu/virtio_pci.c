#include "virtio_pci.h"
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vga.h>

#define PCI_VENDOR_VIRTIO 0x1AF4
#define PCI_DEVICE_GPU_MODERN 0x1050
#define PCI_DEVICE_GPU_TRANS  0x1010 /* 0x1000 + virtio-gpu id 16 */

static inline void mmio_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline uint8_t mmio_r8(volatile uint8_t *p)
{
    return *p;
}

static inline void mmio_w8(volatile uint8_t *p, uint8_t v)
{
    *p = v;
    mmio_barrier();
}

static inline uint16_t mmio_r16(volatile uint8_t *p)
{
    return *(volatile uint16_t *)p;
}

static inline void mmio_w16(volatile uint8_t *p, uint16_t v)
{
    *(volatile uint16_t *)p = v;
    mmio_barrier();
}

static inline uint32_t mmio_r32(volatile uint8_t *p)
{
    return *(volatile uint32_t *)p;
}

static inline void mmio_w32(volatile uint8_t *p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
    mmio_barrier();
}

static volatile uint8_t *map_cap(const pci_device_t *pci, uint8_t bar, uint32_t offset)
{
    uint32_t base = pci_bar_phys(pci, (int)bar);
    if (!base)
        return NULL;
    return (volatile uint8_t *)(uintptr_t)(base + offset);
}

static int parse_caps(virtio_pci_dev_t *vd)
{
    uint16_t status;
    uint8_t cap;

    status = pci_config_read16(vd->pci.bus, vd->pci.slot, vd->pci.func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST))
        return -1;

    cap = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, PCI_CAPABILITY_LIST);
    while (cap) {
        uint8_t id = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, cap);
        if (id == PCI_CAP_ID_VNDR) {
            uint8_t cfg_type = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func,
                                                (uint8_t)(cap + 3));
            uint8_t bar = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func,
                                           (uint8_t)(cap + 4));
            uint32_t offset =
                (uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 8)) |
                     ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 9)) << 8) |
                     ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 10)) << 16) |
                     ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 11)) << 24);

            volatile uint8_t *ptr = map_cap(&vd->pci, bar, offset);
            if (!ptr)
                return -1;

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                vd->common = ptr;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG: {
                uint32_t mult =
                    (uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 16)) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 17)) << 8) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 18)) << 16) |
                    ((uint32_t)pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 19)) << 24);
                vd->notify = ptr;
                vd->notify_off_multiplier = mult;
                break;
            }
            case VIRTIO_PCI_CAP_ISR_CFG:
                vd->isr = ptr;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                vd->device_cfg = ptr;
                break;
            default:
                break;
            }
        }
        cap = pci_config_read8(vd->pci.bus, vd->pci.slot, vd->pci.func, (uint8_t)(cap + 1));
    }

    if (!vd->common || !vd->notify)
        return -1;
    return 0;
}

int virtio_pci_probe_gpu(pci_device_t *out)
{
    if (!out)
        return -1;
    if (pci_find(PCI_VENDOR_VIRTIO, PCI_DEVICE_GPU_MODERN, out) == 0)
        return 0;
    if (pci_find(PCI_VENDOR_VIRTIO, PCI_DEVICE_GPU_TRANS, out) == 0)
        return 0;
    return -1;
}

void virtio_pci_set_status(virtio_pci_dev_t *vd, uint8_t status)
{
    mmio_w8(vd->common + 20, status); /* device_status */
}

uint8_t virtio_pci_get_status(virtio_pci_dev_t *vd)
{
    return mmio_r8(vd->common + 20);
}

int virtio_pci_negotiate_features(virtio_pci_dev_t *vd)
{
    uint32_t feats_lo, feats_hi;
    uint32_t want_lo = 0;
    uint32_t want_hi = (1u << (VIRTIO_F_VERSION_1 - 32));

    mmio_w32(vd->common + 0, 0); /* device_feature_select */
    feats_lo = mmio_r32(vd->common + 4);
    mmio_w32(vd->common + 0, 1);
    feats_hi = mmio_r32(vd->common + 4);

    if (!(feats_hi & want_hi)) {
        vga_print("virtio: no VERSION_1\n");
        return -1;
    }

    (void)feats_lo;
    mmio_w32(vd->common + 8, 0); /* guest_feature_select */
    mmio_w32(vd->common + 12, want_lo);
    mmio_w32(vd->common + 8, 1);
    mmio_w32(vd->common + 12, want_hi);

    virtio_pci_set_status(vd, (uint8_t)(virtio_pci_get_status(vd) | VIRTIO_STATUS_FEATURES_OK));
    if (!(virtio_pci_get_status(vd) & VIRTIO_STATUS_FEATURES_OK)) {
        vga_print("virtio: FEATURES_OK rejected\n");
        return -1;
    }
    return 0;
}

int virtio_pci_init(virtio_pci_dev_t *vd, const pci_device_t *pci)
{
    if (!vd || !pci)
        return -1;
    memset(vd, 0, sizeof(*vd));
    vd->pci = *pci;

    if (pci_enable_bus_master(&vd->pci) < 0)
        return -1;
    if (parse_caps(vd) < 0) {
        vga_print("virtio: modern caps missing\n");
        return -1;
    }

    virtio_pci_set_status(vd, 0); /* reset */
    virtio_pci_set_status(vd, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_pci_set_status(vd, (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER));

    if (virtio_pci_negotiate_features(vd) < 0) {
        virtio_pci_set_status(vd, VIRTIO_STATUS_FAILED);
        return -1;
    }
    return 0;
}

int virtio_pci_setup_queue(virtio_pci_dev_t *vd, uint16_t qindex, uint16_t max_size)
{
    uint16_t qsz, i;
    size_t desc_sz, avail_sz, used_sz, total;
    uint8_t *mem;
    uintptr_t used_addr;

    mmio_w16(vd->common + 22, qindex); /* queue_select */
    qsz = mmio_r16(vd->common + 24);   /* queue_size */
    if (qsz == 0)
        return -1;
    if (qsz > max_size)
        qsz = max_size;
    /* power of two */
    while (qsz & (qsz - 1))
        qsz--;
    if (qsz < 4)
        return -1;
    mmio_w16(vd->common + 24, qsz);

    desc_sz = (size_t)qsz * sizeof(virtq_desc_t);
    avail_sz = 4u + (size_t)qsz * 2u + 2u;
    used_sz = 4u + (size_t)qsz * sizeof(virtq_used_elem_t);
    total = desc_sz + avail_sz;
    total = (total + 3u) & ~3u;
    total += used_sz;
    total = (total + 4095u) & ~4095u;

    mem = (uint8_t *)kmalloc_aligned(total, 4096);
    if (!mem)
        return -1;
    memset(mem, 0, total);

    vd->queue_mem = mem;
    vd->desc = (virtq_desc_t *)mem;
    vd->avail = (virtq_avail_t *)(mem + desc_sz);
    used_addr = ((uintptr_t)mem + desc_sz + avail_sz + 3u) & ~3u;
    vd->used = (virtq_used_t *)used_addr;
    vd->qsize = qsz;
    vd->last_used_idx = 0;

    /* free list */
    vd->free_head = 0;
    vd->num_free = qsz;
    for (i = 0; i < qsz - 1; i++)
        vd->desc[i].next = (uint16_t)(i + 1);
    vd->desc[qsz - 1].next = 0;

    mmio_w32(vd->common + 32, (uint32_t)(uintptr_t)vd->desc);
    mmio_w32(vd->common + 36, 0);
    mmio_w32(vd->common + 40, (uint32_t)(uintptr_t)vd->avail);
    mmio_w32(vd->common + 44, 0);
    mmio_w32(vd->common + 48, (uint32_t)(uintptr_t)vd->used);
    mmio_w32(vd->common + 52, 0);

    vd->queue_notify_off = mmio_r16(vd->common + 30);
    mmio_w16(vd->common + 28, 1); /* queue_enable */
    return 0;
}

static int alloc_desc_chain(virtio_pci_dev_t *vd, uint16_t n, uint16_t *head_out)
{
    uint16_t head, i, cur;
    if (n == 0 || vd->num_free < n)
        return -1;
    head = vd->free_head;
    cur = head;
    for (i = 0; i < (uint16_t)(n - 1); i++) {
        vd->desc[cur].flags = VRING_DESC_F_NEXT;
        cur = vd->desc[cur].next;
    }
    vd->free_head = vd->desc[cur].next;
    vd->desc[cur].flags = 0;
    vd->desc[cur].next = 0;
    vd->num_free = (uint16_t)(vd->num_free - n);
    *head_out = head;
    return 0;
}

static void free_desc_chain(virtio_pci_dev_t *vd, uint16_t head)
{
    uint16_t cur = head;
    for (;;) {
        uint16_t flags = vd->desc[cur].flags;
        uint16_t next = vd->desc[cur].next;
        vd->num_free++;
        if (!(flags & VRING_DESC_F_NEXT)) {
            vd->desc[cur].next = vd->free_head;
            vd->free_head = head;
            break;
        }
        cur = next;
    }
}

static void notify_queue(virtio_pci_dev_t *vd)
{
    uint32_t off = (uint32_t)vd->queue_notify_off * vd->notify_off_multiplier;
    mmio_w16(vd->notify + off, VIRTIO_GPU_QUEUE_CONTROL);
}

int virtio_pci_queue_submit(virtio_pci_dev_t *vd,
                            const void *out, uint32_t out_len,
                            void *in, uint32_t in_len)
{
    uint16_t head, d0, d1;
    uint16_t avail_idx;
    uint32_t spins;

    if (!vd || !out || !out_len || !in || !in_len)
        return -1;
    if (alloc_desc_chain(vd, 2, &head) < 0)
        return -1;

    d0 = head;
    d1 = vd->desc[d0].next;

    vd->desc[d0].addr = (uint64_t)(uintptr_t)out;
    vd->desc[d0].len = out_len;
    vd->desc[d0].flags = VRING_DESC_F_NEXT;
    vd->desc[d0].next = d1;

    vd->desc[d1].addr = (uint64_t)(uintptr_t)in;
    vd->desc[d1].len = in_len;
    vd->desc[d1].flags = VRING_DESC_F_WRITE;
    vd->desc[d1].next = 0;

    avail_idx = vd->avail->idx;
    vd->avail->ring[avail_idx % vd->qsize] = head;
    mmio_barrier();
    vd->avail->idx = (uint16_t)(avail_idx + 1);
    mmio_barrier();

    notify_queue(vd);

    for (spins = 0; spins < 10000000u; spins++) {
        mmio_barrier();
        if (vd->used->idx != vd->last_used_idx) {
            uint16_t u = vd->last_used_idx % vd->qsize;
            uint16_t id = (uint16_t)vd->used->ring[u].id;
            vd->last_used_idx++;
            free_desc_chain(vd, id);
            if (vd->isr)
                (void)mmio_r8(vd->isr); /* ack */
            return 0;
        }
    }

    vga_print("virtio: queue timeout\n");
    free_desc_chain(vd, head);
    return -1;
}
