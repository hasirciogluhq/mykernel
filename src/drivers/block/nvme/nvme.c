#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/pci.h>

#define NVME_MAX_CONTROLLERS 4
#define NVME_MAX_NAMESPACES  16
#define NVME_QUEUE_MAX       64
#define NVME_MAX_XFER        (2u * 1024u * 1024u)

#define NVME_REG_CAP         0x0000
#define NVME_REG_CC          0x0014
#define NVME_REG_CSTS        0x001C
#define NVME_REG_AQA         0x0024
#define NVME_REG_ASQ         0x0028
#define NVME_REG_ACQ         0x0030
#define NVME_REG_DBS         0x1000

#define NVME_CC_EN           (1u << 0)
#define NVME_CSTS_RDY        (1u << 0)
#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_CMD_WRITE       0x01
#define NVME_CMD_READ        0x02

typedef struct {
    uint32_t dword[16];
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

typedef struct {
    bio_t *bio;
    uint64_t *prp_list;
    int used;
} nvme_pending_t;

typedef struct nvme_controller nvme_controller_t;

typedef struct {
    nvme_controller_t *ctrl;
    uint32_t nsid;
    block_device_t bdev;
} nvme_namespace_t;

struct nvme_controller {
    pci_device_t pci;
    volatile uint8_t *regs;
    uint32_t db_stride;
    uint16_t qdepth;
    uint16_t admin_tail;
    uint16_t admin_head;
    uint16_t admin_phase;
    uint16_t io_tail;
    uint16_t io_head;
    uint16_t io_phase;
    nvme_cmd_t *admin_sq;
    volatile nvme_cqe_t *admin_cq;
    nvme_cmd_t *io_sq;
    volatile nvme_cqe_t *io_cq;
    nvme_pending_t pending[NVME_QUEUE_MAX];
};

typedef struct {
    pci_device_t devices[NVME_MAX_CONTROLLERS];
    size_t count;
} nvme_scan_t;

static nvme_controller_t *g_nvme_controllers[NVME_MAX_CONTROLLERS];
static size_t g_nvme_controller_count;

static inline uint32_t mmio_r32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(base + off) = value;
}

static inline void mmio_w64(volatile uint8_t *base, uint32_t off, uint64_t value)
{
    mmio_w32(base, off, (uint32_t)value);
    mmio_w32(base, off + 4, (uint32_t)(value >> 32));
}

static inline uint64_t mmio_r64(volatile uint8_t *base, uint32_t off)
{
    uint64_t lo = mmio_r32(base, off);
    return lo | ((uint64_t)mmio_r32(base, off + 4) << 32);
}

static void nvme_complete(bio_t *bio, int error)
{
    if (!bio)
        return;
    bio->error = error;
    bio->done = 1;
    if (bio->end_io)
        bio->end_io(bio);
}

static volatile uint8_t *nvme_doorbell(nvme_controller_t *ctrl, uint16_t qid, int cq)
{
    return ctrl->regs + NVME_REG_DBS + ((uint32_t)qid * 2u + (cq ? 1u : 0u)) *
                        ctrl->db_stride;
}

static int nvme_wait_ready(nvme_controller_t *ctrl, int ready)
{
    int spin;
    for (spin = 0; spin < 1000000; spin++) {
        if (!!(mmio_r32(ctrl->regs, NVME_REG_CSTS) & NVME_CSTS_RDY) == !!ready)
            return 0;
    }
    return -EIO;
}

static int nvme_admin_command(nvme_controller_t *ctrl, nvme_cmd_t *cmd)
{
    volatile nvme_cqe_t *cqe;
    uint16_t tail;
    int spin;

    cmd->dword[0] &= 0x0000FFFFu;
    ctrl->admin_sq[ctrl->admin_tail] = *cmd;
    __asm__ volatile("" ::: "memory");
    tail = (uint16_t)((ctrl->admin_tail + 1u) % ctrl->qdepth);
    mmio_w32(nvme_doorbell(ctrl, 0, 0), 0, tail);
    ctrl->admin_tail = tail;
    for (spin = 0; spin < 1000000; spin++) {
        cqe = &ctrl->admin_cq[ctrl->admin_head];
        if ((cqe->status & 1u) != ctrl->admin_phase)
            continue;
        if (cqe->cid != 0)
            return -EIO;
        ctrl->admin_head = (uint16_t)((ctrl->admin_head + 1u) % ctrl->qdepth);
        if (!ctrl->admin_head)
            ctrl->admin_phase ^= 1u;
        mmio_w32(nvme_doorbell(ctrl, 0, 1), 0, ctrl->admin_head);
        return (cqe->status >> 1) ? -EIO : 0;
    }
    return -EIO;
}

static int nvme_identify(nvme_controller_t *ctrl, uint32_t nsid, uint8_t cns, void *buffer)
{
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dword[0] = NVME_ADMIN_IDENTIFY;
    cmd.dword[1] = nsid;
    cmd.dword[6] = (uint32_t)(uintptr_t)buffer;
    cmd.dword[7] = 0;
    cmd.dword[10] = cns;
    return nvme_admin_command(ctrl, &cmd);
}

static int nvme_create_io_queues(nvme_controller_t *ctrl)
{
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dword[0] = NVME_ADMIN_CREATE_CQ;
    cmd.dword[6] = (uint32_t)(uintptr_t)ctrl->io_cq;
    cmd.dword[10] = 1u | ((uint32_t)(ctrl->qdepth - 1u) << 16);
    cmd.dword[11] = 1u; /* physically contiguous completion queue */
    if (nvme_admin_command(ctrl, &cmd) < 0)
        return -EIO;

    memset(&cmd, 0, sizeof(cmd));
    cmd.dword[0] = NVME_ADMIN_CREATE_SQ;
    cmd.dword[6] = (uint32_t)(uintptr_t)ctrl->io_sq;
    cmd.dword[10] = 1u | ((uint32_t)(ctrl->qdepth - 1u) << 16);
    cmd.dword[11] = 1u;
    return nvme_admin_command(ctrl, &cmd);
}

static int nvme_poll_controller(nvme_controller_t *ctrl)
{
    int completed = 0;
    int consumed = 0;
    while (1) {
        volatile nvme_cqe_t *cqe = &ctrl->io_cq[ctrl->io_head];
        uint16_t cid;
        int error;
        if ((cqe->status & 1u) != ctrl->io_phase)
            break;
        consumed = 1;
        cid = cqe->cid;
        error = (cqe->status >> 1) ? -EIO : 0;
        if (cid < ctrl->qdepth && ctrl->pending[cid].used) {
            bio_t *bio = ctrl->pending[cid].bio;
            ctrl->pending[cid].used = 0;
            ctrl->pending[cid].bio = NULL;
            nvme_complete(bio, error);
            completed++;
        }
        ctrl->io_head = (uint16_t)((ctrl->io_head + 1u) % ctrl->qdepth);
        if (!ctrl->io_head)
            ctrl->io_phase ^= 1u;
    }
    if (consumed)
        mmio_w32(nvme_doorbell(ctrl, 1, 1), 0, ctrl->io_head);
    return completed;
}

static int nvme_submit(bio_t *bio)
{
    nvme_namespace_t *ns;
    nvme_controller_t *ctrl;
    nvme_cmd_t *cmd;
    nvme_pending_t *pending;
    uintptr_t address;
    size_t first;
    size_t left;
    uint32_t sectors;
    uint32_t pages = 0;
    int cid;

    if (!bio || !bio->bdev || !bio->data || !bio->len || (bio->len & 511u) ||
        bio->len > NVME_MAX_XFER)
        return -EINVAL;
    sectors = (uint32_t)(bio->len / 512u);
    if (!sectors || sectors > 65536u || bio->sector > bio->bdev->capacity ||
        sectors > bio->bdev->capacity - bio->sector)
        return -EINVAL;
    ns = (nvme_namespace_t *)bio->bdev->private_data;
    if (!ns || !ns->ctrl)
        return -EIO;
    ctrl = ns->ctrl;
    for (cid = 0; cid < ctrl->qdepth; cid++) {
        if (!ctrl->pending[cid].used)
            break;
    }
    if (cid == ctrl->qdepth)
        return -EAGAIN;

    address = (uintptr_t)bio->data;
    first = 4096u - (address & 4095u);
    if (first > bio->len)
        first = bio->len;
    left = bio->len - first;
    pending = &ctrl->pending[cid];
    if (left > 4096u) {
        uintptr_t page = (address + first + 4095u) & ~4095u;
        while (left) {
            if (pages == 512u)
                return -EINVAL;
            pending->prp_list[pages++] = (uint64_t)page;
            page += 4096u;
            left = left > 4096u ? left - 4096u : 0;
        }
    }

    cmd = &ctrl->io_sq[ctrl->io_tail];
    memset(cmd, 0, sizeof(*cmd));
    cmd->dword[0] = (bio->write ? NVME_CMD_WRITE : NVME_CMD_READ) | ((uint32_t)cid << 16);
    cmd->dword[1] = ns->nsid;
    cmd->dword[6] = (uint32_t)address;
    cmd->dword[7] = 0;
    if (bio->len > first) {
        if (bio->len - first <= 4096u) {
            cmd->dword[8] = (uint32_t)((address + first + 4095u) & ~4095u);
            cmd->dword[9] = 0;
        } else {
            cmd->dword[8] = (uint32_t)(uintptr_t)pending->prp_list;
            cmd->dword[9] = 0;
        }
    }
    cmd->dword[10] = (uint32_t)bio->sector;
    cmd->dword[11] = (uint32_t)(bio->sector >> 32);
    cmd->dword[12] = sectors - 1u;
    pending->bio = bio;
    pending->used = 1;
    __asm__ volatile("" ::: "memory");
    ctrl->io_tail = (uint16_t)((ctrl->io_tail + 1u) % ctrl->qdepth);
    mmio_w32(nvme_doorbell(ctrl, 1, 0), 0, ctrl->io_tail);
    return 0;
}

static int nvme_poll(block_device_t *bdev)
{
    nvme_namespace_t *ns = bdev ? (nvme_namespace_t *)bdev->private_data : NULL;
    return ns ? nvme_poll_controller(ns->ctrl) : 0;
}

static const block_ops_t nvme_ops = {
    .submit_bio = nvme_submit,
    .poll = nvme_poll,
    .ioctl = NULL,
};

static void nvme_driver_poll(driver_t *drv)
{
    size_t i;
    (void)drv;
    for (i = 0; i < g_nvme_controller_count; i++)
        nvme_poll_controller(g_nvme_controllers[i]);
}

static void nvme_append_uint(char *out, size_t *pos, uint32_t value)
{
    char digits[10];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n)
        out[(*pos)++] = digits[--n];
}

static int nvme_register_namespace(nvme_controller_t *ctrl, uint32_t nsid,
                                   const block_api_t *api, size_t controller_index)
{
    uint8_t *identify;
    uint64_t capacity;
    nvme_namespace_t *ns;
    size_t pos = 0;

    identify = (uint8_t *)kmalloc_aligned(4096, 4096);
    if (!identify)
        return -ENOMEM;
    memset(identify, 0, 4096);
    if (nvme_identify(ctrl, nsid, 0, identify) < 0)
        return 0; /* Namespace IDs may be allocated but inactive. */
    capacity = (uint64_t)identify[0] |
               ((uint64_t)identify[1] << 8) |
               ((uint64_t)identify[2] << 16) |
               ((uint64_t)identify[3] << 24) |
               ((uint64_t)identify[4] << 32) |
               ((uint64_t)identify[5] << 40) |
               ((uint64_t)identify[6] << 48) |
               ((uint64_t)identify[7] << 56);
    if (!capacity)
        return 0;
    {
        uint8_t format = identify[26] & 0x0Fu;
        if (identify[128u + (uint32_t)format * 4u + 2u] != 9u)
            return -ENOTSUP;
    }
    ns = (nvme_namespace_t *)kmalloc(sizeof(*ns));
    if (!ns)
        return -ENOMEM;
    memset(ns, 0, sizeof(*ns));
    ns->ctrl = ctrl;
    ns->nsid = nsid;
    strcpy(ns->bdev.name, "nvme");
    pos = 4;
    nvme_append_uint(ns->bdev.name, &pos, (uint32_t)controller_index);
    ns->bdev.name[pos++] = 'n';
    nvme_append_uint(ns->bdev.name, &pos, nsid);
    ns->bdev.name[pos] = 0;
    ns->bdev.capacity = capacity;
    ns->bdev.sector_size = 512;
    ns->bdev.ops = &nvme_ops;
    ns->bdev.private_data = ns;
    ns->bdev.major = 259;
    ns->bdev.minor = (int)(controller_index * NVME_MAX_NAMESPACES + nsid - 1u);
    if (api->add_disk(&ns->bdev) < 0)
        return -EIO;
    if (api->scan_partitions)
        api->scan_partitions(&ns->bdev);
    return 1;
}

static int nvme_init_controller(const pci_device_t *pci, const block_api_t *api,
                                size_t controller_index)
{
    nvme_controller_t *ctrl;
    uint64_t cap;
    uint8_t *identify;
    uint32_t nn;
    uint32_t nsid;
    uint32_t mqes;
    uint32_t max_ns;

    ctrl = (nvme_controller_t *)kmalloc(sizeof(*ctrl));
    if (!ctrl)
        return -ENOMEM;
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pci = *pci;
    ctrl->regs = (volatile uint8_t *)(uintptr_t)pci_bar_phys(pci, 0);
    if (!ctrl->regs || pci_enable_bus_master(pci) < 0)
        return -EIO;
    cap = mmio_r64(ctrl->regs, NVME_REG_CAP);
    if (((cap >> 48) & 0xFu) != 0)
        return -ENOTSUP;
    mqes = (uint32_t)(cap & 0xFFFFu) + 1u;
    ctrl->qdepth = (uint16_t)(mqes > NVME_QUEUE_MAX ? NVME_QUEUE_MAX : mqes);
    if (ctrl->qdepth < 2)
        return -EIO;
    ctrl->db_stride = 4u << ((cap >> 32) & 0xFu);

    mmio_w32(ctrl->regs, NVME_REG_CC, 0);
    if (nvme_wait_ready(ctrl, 0) < 0)
        return -EIO;
    ctrl->admin_sq = (nvme_cmd_t *)kmalloc_aligned(4096, 4096);
    ctrl->admin_cq = (volatile nvme_cqe_t *)kmalloc_aligned(4096, 4096);
    ctrl->io_sq = (nvme_cmd_t *)kmalloc_aligned(4096, 4096);
    ctrl->io_cq = (volatile nvme_cqe_t *)kmalloc_aligned(4096, 4096);
    if (!ctrl->admin_sq || !ctrl->admin_cq || !ctrl->io_sq || !ctrl->io_cq)
        return -ENOMEM;
    memset(ctrl->admin_sq, 0, 4096);
    memset((void *)ctrl->admin_cq, 0, 4096);
    memset(ctrl->io_sq, 0, 4096);
    memset((void *)ctrl->io_cq, 0, 4096);
    for (nsid = 0; nsid < ctrl->qdepth; nsid++) {
        ctrl->pending[nsid].prp_list = (uint64_t *)kmalloc_aligned(4096, 4096);
        if (!ctrl->pending[nsid].prp_list)
            return -ENOMEM;
        memset(ctrl->pending[nsid].prp_list, 0, 4096);
    }
    ctrl->admin_phase = 1;
    ctrl->io_phase = 1;
    mmio_w32(ctrl->regs, NVME_REG_AQA,
             (uint32_t)(ctrl->qdepth - 1u) | ((uint32_t)(ctrl->qdepth - 1u) << 16));
    mmio_w64(ctrl->regs, NVME_REG_ASQ, (uint64_t)(uintptr_t)ctrl->admin_sq);
    mmio_w64(ctrl->regs, NVME_REG_ACQ, (uint64_t)(uintptr_t)ctrl->admin_cq);
    mmio_w32(ctrl->regs, NVME_REG_CC, NVME_CC_EN | (6u << 16) | (4u << 20));
    if (nvme_wait_ready(ctrl, 1) < 0 || nvme_create_io_queues(ctrl) < 0)
        return -EIO;

    identify = (uint8_t *)kmalloc_aligned(4096, 4096);
    if (!identify)
        return -ENOMEM;
    memset(identify, 0, 4096);
    if (nvme_identify(ctrl, 0, 1, identify) < 0)
        return -EIO;
    nn = (uint32_t)identify[516] | ((uint32_t)identify[517] << 8) |
         ((uint32_t)identify[518] << 16) | ((uint32_t)identify[519] << 24);
    max_ns = nn > NVME_MAX_NAMESPACES ? NVME_MAX_NAMESPACES : nn;
    for (nsid = 1; nsid <= max_ns; nsid++) {
        int rc = nvme_register_namespace(ctrl, nsid, api, controller_index);
        if (rc < 0)
            return rc;
    }
    if (g_nvme_controller_count < NVME_MAX_CONTROLLERS)
        g_nvme_controllers[g_nvme_controller_count++] = ctrl;
    return 0;
}

static int nvme_scan_cb(const pci_device_t *dev, void *ctx)
{
    nvme_scan_t *scan = (nvme_scan_t *)ctx;
    if (dev->class_code == 0x01 && dev->subclass == 0x08) {
        if (scan->count < NVME_MAX_CONTROLLERS)
            scan->devices[scan->count++] = *dev;
    }
    return 0;
}

static int nvme_probe_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    nvme_scan_t scan;
    size_t i;
    (void)drv;
    (void)ctx;
    if (!api || !api->add_disk)
        return -1;
    memset(&scan, 0, sizeof(scan));
    pci_enumerate(nvme_scan_cb, &scan);
    if (!scan.count) {
        vga_print("nvme: no controller\n");
        return 0;
    }
    for (i = 0; i < scan.count; i++) {
        if (nvme_init_controller(&scan.devices[i], api, i) < 0)
            return -1;
    }
    vga_print("nvme: controller ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "nvme", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BLOCK;
    d.flags = DRIVER_FLAG_POLL;
    d.priority = 81;
    d.init = nvme_probe_init;
    d.poll = nvme_driver_poll;
    if (driver_register(&d) < 0)
        return -1;
    return driver_load("nvme", NULL) < 0 ? -1 : 0;
}
