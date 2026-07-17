#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/pci.h>

#define AHCI_MAX_CONTROLLERS 4
#define AHCI_PORTS           32
#define AHCI_SLOTS           32
#define AHCI_PRDT_MAX        8
#define AHCI_PRDT_BYTES      (4u * 1024u * 1024u)
#define AHCI_TABLE_BYTES     4096u

#define HBA_GHC              0x04
#define HBA_PI               0x0C
#define HBA_GHC_HR           (1u << 0)
#define HBA_GHC_AE           (1u << 31)

#define PX_CLB               0x00
#define PX_CLBU              0x04
#define PX_FB                0x08
#define PX_FBU               0x0C
#define PX_IS                0x10
#define PX_CMD               0x18
#define PX_TFD               0x20
#define PX_SSTS              0x28
#define PX_SERR              0x30
#define PX_CI                0x38

#define PX_CMD_ST            (1u << 0)
#define PX_CMD_FRE           (1u << 4)
#define PX_CMD_FR            (1u << 14)
#define PX_CMD_CR            (1u << 15)
#define PX_IS_TFES           (1u << 30)

#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_ioc;
} __attribute__((packed)) ahci_prdt_t;

typedef struct {
    uint8_t  flags;
    uint8_t  prdtl;
    uint16_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport_c;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    fis_reg_h2d_t cfis;
    uint8_t acmd[16];
    uint8_t reserved[48];
    ahci_prdt_t prdt[AHCI_PRDT_MAX];
} __attribute__((packed)) ahci_cmd_table_t;

typedef struct {
    bio_t *bio;
    int *result;
    int used;
} ahci_pending_t;

typedef struct ahci_port {
    volatile uint8_t *regs;
    ahci_cmd_header_t *cmd_list;
    uint8_t *fis;
    uint8_t *tables;
    ahci_pending_t pending[AHCI_SLOTS];
    block_device_t bdev;
} ahci_port_t;

typedef struct {
    pci_device_t pci;
    volatile uint8_t *abar;
    ahci_port_t *ports[AHCI_PORTS];
} ahci_controller_t;

typedef struct {
    pci_device_t devices[AHCI_MAX_CONTROLLERS];
    size_t count;
} ahci_scan_t;

static ahci_port_t *g_ahci_ports[AHCI_MAX_CONTROLLERS * AHCI_PORTS];
static size_t g_ahci_port_count;

static inline uint32_t mmio_r32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(base + off) = value;
}

static void ahci_complete(bio_t *bio, int error)
{
    if (!bio)
        return;
    bio->error = error;
    bio->done = 1;
    if (bio->end_io)
        bio->end_io(bio);
}

static int ahci_stop_engine(ahci_port_t *port)
{
    uint32_t cmd = mmio_r32(port->regs, PX_CMD);
    int spin;

    mmio_w32(port->regs, PX_CMD, cmd & ~(PX_CMD_ST | PX_CMD_FRE));
    for (spin = 0; spin < 1000000; spin++) {
        cmd = mmio_r32(port->regs, PX_CMD);
        if (!(cmd & (PX_CMD_CR | PX_CMD_FR)))
            return 0;
    }
    return -EIO;
}

static int ahci_start_engine(ahci_port_t *port)
{
    uint32_t cmd;
    if (ahci_stop_engine(port) < 0)
        return -EIO;
    mmio_w32(port->regs, PX_IS, 0xFFFFFFFFu);
    mmio_w32(port->regs, PX_SERR, 0xFFFFFFFFu);
    cmd = mmio_r32(port->regs, PX_CMD);
    mmio_w32(port->regs, PX_CMD, cmd | PX_CMD_FRE | PX_CMD_ST);
    return 0;
}

static int ahci_port_poll(ahci_port_t *port)
{
    uint32_t is;
    uint32_t ci;
    int slot;
    int completed = 0;

    if (!port)
        return 0;
    is = mmio_r32(port->regs, PX_IS);
    ci = mmio_r32(port->regs, PX_CI);
    if (is)
        mmio_w32(port->regs, PX_IS, is);
    for (slot = 0; slot < AHCI_SLOTS; slot++) {
        ahci_pending_t *pending = &port->pending[slot];
        int error;
        if (!pending->used || (ci & (1u << slot)))
            continue;
        error = (is & PX_IS_TFES) ? -EIO : 0;
        pending->used = 0;
        if (pending->result)
            *pending->result = error;
        ahci_complete(pending->bio, error);
        completed++;
    }
    return completed;
}

static int ahci_issue(ahci_port_t *port, bio_t *bio, void *data, size_t len,
                      uint64_t lba, uint32_t sectors, int write,
                      uint8_t command, int *result)
{
    ahci_cmd_header_t *hdr;
    ahci_cmd_table_t *table;
    uintptr_t addr;
    size_t left;
    uint32_t prdtl = 0;
    int slot;
    int spin;

    if (!port || !data || !len || !sectors || sectors > 65536u ||
        len > AHCI_PRDT_MAX * AHCI_PRDT_BYTES)
        return -EINVAL;
    for (slot = 0; slot < AHCI_SLOTS; slot++) {
        if (!port->pending[slot].used)
            break;
    }
    if (slot == AHCI_SLOTS)
        return -EAGAIN;
    for (spin = 0; spin < 1000000; spin++) {
        if (!(mmio_r32(port->regs, PX_TFD) & 0x88u))
            break;
    }
    if (spin == 1000000)
        return -EIO;

    hdr = &port->cmd_list[slot];
    table = (ahci_cmd_table_t *)(port->tables + (size_t)slot * AHCI_TABLE_BYTES);
    memset(hdr, 0, sizeof(*hdr));
    memset(table, 0, AHCI_TABLE_BYTES);

    addr = (uintptr_t)data;
    left = len;
    while (left) {
        size_t chunk = left > AHCI_PRDT_BYTES ? AHCI_PRDT_BYTES : left;
        if (prdtl == AHCI_PRDT_MAX)
            return -EINVAL;
        table->prdt[prdtl].dba = (uint32_t)addr;
        table->prdt[prdtl].dbau = 0;
        table->prdt[prdtl].dbc_ioc = ((uint32_t)chunk - 1u) |
                                     ((left == chunk) ? (1u << 31) : 0);
        addr += chunk;
        left -= chunk;
        prdtl++;
    }

    table->cfis.fis_type = 0x27;
    table->cfis.pmport_c = 1u << 7;
    table->cfis.command = command;
    table->cfis.device = 1u << 6;
    table->cfis.lba0 = (uint8_t)lba;
    table->cfis.lba1 = (uint8_t)(lba >> 8);
    table->cfis.lba2 = (uint8_t)(lba >> 16);
    table->cfis.lba3 = (uint8_t)(lba >> 24);
    table->cfis.lba4 = (uint8_t)(lba >> 32);
    table->cfis.lba5 = (uint8_t)(lba >> 40);
    table->cfis.countl = (uint8_t)sectors;
    table->cfis.counth = (uint8_t)(sectors >> 8);
    hdr->flags = (uint8_t)(5u | (write ? (1u << 6) : 0));
    hdr->prdtl = (uint8_t)prdtl;
    hdr->ctba = (uint32_t)(uintptr_t)table;

    port->pending[slot].bio = bio;
    port->pending[slot].result = result;
    port->pending[slot].used = 1;
    __asm__ volatile("" ::: "memory");
    mmio_w32(port->regs, PX_CI, mmio_r32(port->regs, PX_CI) | (1u << slot));
    return 0;
}

static int ahci_submit(bio_t *bio)
{
    ahci_port_t *port;
    uint32_t sectors;
    if (!bio || !bio->bdev || !bio->data || !bio->len || (bio->len & 511u))
        return -EINVAL;
    sectors = (uint32_t)(bio->len / 512u);
    if (!sectors || sectors > 65536u || bio->sector > bio->bdev->capacity ||
        sectors > bio->bdev->capacity - bio->sector)
        return -EINVAL;
    port = (ahci_port_t *)bio->bdev->private_data;
    return ahci_issue(port, bio, bio->data, bio->len, bio->sector, sectors,
                      bio->write, bio->write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                      NULL);
}

static int ahci_poll(block_device_t *bdev)
{
    ahci_port_t *port = bdev ? (ahci_port_t *)bdev->private_data : NULL;
    return ahci_port_poll(port);
}

static const block_ops_t ahci_ops = {
    .submit_bio = ahci_submit,
    .poll = ahci_poll,
    .ioctl = NULL,
};

static void ahci_driver_poll(driver_t *drv)
{
    size_t i;
    (void)drv;
    for (i = 0; i < g_ahci_port_count; i++)
        ahci_port_poll(g_ahci_ports[i]);
}

static int ahci_identify(ahci_port_t *port, uint64_t *capacity)
{
    uint16_t *identify;
    int result = 1;
    int spin;

    identify = (uint16_t *)kmalloc_aligned(512, 512);
    if (!identify)
        return -ENOMEM;
    memset(identify, 0, 512);
    if (ahci_issue(port, NULL, identify, 512, 0, 1, 0, ATA_CMD_IDENTIFY, &result) < 0)
        return -EIO;
    for (spin = 0; spin < 1000000 && result == 1; spin++)
        ahci_port_poll(port);
    if (result == 1)
        return -EIO;
    if (result != 0)
        return result;
    *capacity = (uint64_t)identify[100] |
                ((uint64_t)identify[101] << 16) |
                ((uint64_t)identify[102] << 32) |
                ((uint64_t)identify[103] << 48);
    return *capacity ? 0 : -EIO;
}

static void ahci_name(char *name, size_t index)
{
    char suffix[8];
    size_t n = 0;
    size_t i;
    strcpy(name, "sd");
    do {
        suffix[n++] = (char)('a' + (index % 26u));
        index = index / 26u;
        if (index)
            index--;
    } while (index && n < sizeof(suffix));
    for (i = 0; i < n; i++)
        name[2 + i] = suffix[n - i - 1];
    name[2 + n] = 0;
}

static int ahci_init_port(ahci_controller_t *ctrl, uint32_t portno,
                          const block_api_t *api, size_t *disk_index)
{
    ahci_port_t *port;
    uint32_t ssts;
    uint64_t capacity;

    ssts = mmio_r32(ctrl->abar + 0x100u + portno * 0x80u, PX_SSTS);
    if ((ssts & 0x0Fu) != 3u)
        return 0;
    port = (ahci_port_t *)kmalloc(sizeof(*port));
    if (!port)
        return -ENOMEM;
    memset(port, 0, sizeof(*port));
    port->regs = ctrl->abar + 0x100u + portno * 0x80u;
    port->cmd_list = (ahci_cmd_header_t *)kmalloc_aligned(1024, 1024);
    port->fis = (uint8_t *)kmalloc_aligned(256, 256);
    port->tables = (uint8_t *)kmalloc_aligned(AHCI_SLOTS * AHCI_TABLE_BYTES, 128);
    if (!port->cmd_list || !port->fis || !port->tables)
        return -ENOMEM;
    memset(port->cmd_list, 0, 1024);
    memset(port->fis, 0, 256);
    memset(port->tables, 0, AHCI_SLOTS * AHCI_TABLE_BYTES);
    if (ahci_stop_engine(port) < 0)
        return -EIO;
    mmio_w32(port->regs, PX_CLB, (uint32_t)(uintptr_t)port->cmd_list);
    mmio_w32(port->regs, PX_CLBU, 0);
    mmio_w32(port->regs, PX_FB, (uint32_t)(uintptr_t)port->fis);
    mmio_w32(port->regs, PX_FBU, 0);
    if (ahci_start_engine(port) < 0 || ahci_identify(port, &capacity) < 0)
        return -EIO;

    memset(&port->bdev, 0, sizeof(port->bdev));
    ahci_name(port->bdev.name, *disk_index);
    port->bdev.capacity = capacity;
    port->bdev.sector_size = 512;
    port->bdev.ops = &ahci_ops;
    port->bdev.private_data = port;
    port->bdev.major = 8;
    port->bdev.minor = (int)*disk_index;
    if (api->add_disk(&port->bdev) < 0)
        return -EIO;
    if (api->scan_partitions)
        api->scan_partitions(&port->bdev);
    ctrl->ports[portno] = port;
    if (g_ahci_port_count < AHCI_MAX_CONTROLLERS * AHCI_PORTS)
        g_ahci_ports[g_ahci_port_count++] = port;
    (*disk_index)++;
    return 1;
}

static int ahci_init_controller(const pci_device_t *pci, const block_api_t *api,
                                size_t *disk_index)
{
    ahci_controller_t *ctrl;
    uint32_t pi;
    uint32_t portno;
    int spin;

    ctrl = (ahci_controller_t *)kmalloc(sizeof(*ctrl));
    if (!ctrl)
        return -ENOMEM;
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pci = *pci;
    ctrl->abar = (volatile uint8_t *)(uintptr_t)pci_bar_phys(pci, 5);
    if (!ctrl->abar || pci_enable_bus_master(pci) < 0)
        return -EIO;
    mmio_w32(ctrl->abar, HBA_GHC, mmio_r32(ctrl->abar, HBA_GHC) | HBA_GHC_AE | HBA_GHC_HR);
    for (spin = 0; spin < 1000000; spin++) {
        if (!(mmio_r32(ctrl->abar, HBA_GHC) & HBA_GHC_HR))
            break;
    }
    if (spin == 1000000)
        return -EIO;
    mmio_w32(ctrl->abar, HBA_GHC, mmio_r32(ctrl->abar, HBA_GHC) | HBA_GHC_AE);
    pi = mmio_r32(ctrl->abar, HBA_PI);
    for (portno = 0; portno < AHCI_PORTS; portno++) {
        int rc;
        if (!(pi & (1u << portno)))
            continue;
        rc = ahci_init_port(ctrl, portno, api, disk_index);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int ahci_scan_cb(const pci_device_t *dev, void *ctx)
{
    ahci_scan_t *scan = (ahci_scan_t *)ctx;
    if (dev->class_code == 0x01 && dev->subclass == 0x06 && dev->prog_if == 0x01) {
        if (scan->count < AHCI_MAX_CONTROLLERS)
            scan->devices[scan->count++] = *dev;
    }
    return 0;
}

static int ahci_probe_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    ahci_scan_t scan;
    size_t i;
    size_t disk_index = 0;
    (void)drv;
    (void)ctx;
    if (!api || !api->add_disk)
        return -1;
    memset(&scan, 0, sizeof(scan));
    pci_enumerate(ahci_scan_cb, &scan);
    if (!scan.count) {
        vga_print("ahci: no controller\n");
        return 0;
    }
    for (i = 0; i < scan.count; i++) {
        if (ahci_init_controller(&scan.devices[i], api, &disk_index) < 0)
            return -1;
    }
    vga_print("ahci: controller ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "ahci", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BLOCK;
    d.flags = DRIVER_FLAG_POLL;
    d.priority = 80;
    d.init = ahci_probe_init;
    d.poll = ahci_driver_poll;
    if (driver_register(&d) < 0)
        return -1;
    return driver_load("ahci", NULL) < 0 ? -1 : 0;
}
