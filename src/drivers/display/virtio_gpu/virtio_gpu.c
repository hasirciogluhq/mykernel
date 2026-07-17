#include "virtio_gpu.h"
#include "virtio_pci.h"
#include <drivers/display.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <kernel/heap.h>
#include <kernel/string.h>

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_MAX_SCANOUTS          16
#define VIRTIO_GPU_RESOURCE_ID           1
#define VIRTIO_GPU_DEFAULT_W             800
#define VIRTIO_GPU_DEFAULT_H             600

typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

typedef struct virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

typedef struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

typedef struct virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

typedef struct virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

typedef struct virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

typedef struct virtio_gpu_display_one {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

typedef struct attach_req {
    virtio_gpu_resource_attach_backing_t ab;
    virtio_gpu_mem_entry_t entry;
} __attribute__((packed)) attach_req_t;

static virtio_pci_dev_t g_vd;
static display_mode_t g_mode;
static display_ops_t g_ops;
static uint32_t *g_fb;
static uint32_t g_fb_bytes;
static int g_ready;
static int g_device_present;
static pci_device_t g_pci;

static void hdr_init(virtio_gpu_ctrl_hdr_t *h, uint32_t type)
{
    memset(h, 0, sizeof(*h));
    h->type = type;
}

static int gpu_cmd(const void *req, uint32_t req_len, void *resp, uint32_t resp_len)
{
    virtio_gpu_ctrl_hdr_t *rh;
    if (virtio_pci_queue_submit(&g_vd, req, req_len, resp, resp_len) < 0)
        return -1;
    rh = (virtio_gpu_ctrl_hdr_t *)resp;
    if (rh->type < VIRTIO_GPU_RESP_OK_NODATA || rh->type >= 0x1200)
        return -1;
    return 0;
}

static int gpu_cmd_nodata(const void *req, uint32_t req_len)
{
    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));
    if (gpu_cmd(req, req_len, &resp, sizeof(resp)) < 0)
        return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

static int gpu_get_display_size(uint32_t *w, uint32_t *h)
{
    virtio_gpu_ctrl_hdr_t req;
    virtio_gpu_resp_display_info_t resp;
    int i;

    hdr_init(&req, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    memset(&resp, 0, sizeof(resp));
    if (gpu_cmd(&req, sizeof(req), &resp, sizeof(resp)) < 0)
        return -1;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return -1;

    for (i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (resp.pmodes[i].enabled && resp.pmodes[i].r.width && resp.pmodes[i].r.height) {
            *w = resp.pmodes[i].r.width;
            *h = resp.pmodes[i].r.height;
            return 0;
        }
    }
    *w = VIRTIO_GPU_DEFAULT_W;
    *h = VIRTIO_GPU_DEFAULT_H;
    return 0;
}

static int gpu_setup_scanout(uint32_t width, uint32_t height)
{
    virtio_gpu_resource_create_2d_t create;
    attach_req_t attach;
    virtio_gpu_set_scanout_t scan;

    hdr_init(&create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    create.resource_id = VIRTIO_GPU_RESOURCE_ID;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create.width = width;
    create.height = height;
    if (gpu_cmd_nodata(&create, sizeof(create)) < 0) {
        vga_print("virtio-gpu: CREATE_2D failed\n");
        return -1;
    }

    memset(&attach, 0, sizeof(attach));
    hdr_init(&attach.ab.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    attach.ab.resource_id = VIRTIO_GPU_RESOURCE_ID;
    attach.ab.nr_entries = 1;
    attach.entry.addr = (uint64_t)(uintptr_t)g_fb;
    attach.entry.length = g_fb_bytes;
    if (gpu_cmd_nodata(&attach, sizeof(attach)) < 0) {
        vga_print("virtio-gpu: ATTACH_BACKING failed\n");
        return -1;
    }

    memset(&scan, 0, sizeof(scan));
    hdr_init(&scan.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    scan.r.x = 0;
    scan.r.y = 0;
    scan.r.width = width;
    scan.r.height = height;
    scan.scanout_id = 0;
    scan.resource_id = VIRTIO_GPU_RESOURCE_ID;
    if (gpu_cmd_nodata(&scan, sizeof(scan)) < 0) {
        vga_print("virtio-gpu: SET_SCANOUT failed\n");
        return -1;
    }
    return 0;
}

static int gpu_transfer_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virtio_gpu_transfer_to_host_2d_t xfer;
    virtio_gpu_resource_flush_t flush;

    if (w == 0 || h == 0)
        return 0;

    memset(&xfer, 0, sizeof(xfer));
    hdr_init(&xfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    xfer.r.x = x;
    xfer.r.y = y;
    xfer.r.width = w;
    xfer.r.height = h;
    xfer.offset = ((uint64_t)y * g_mode.width + x) * 4u;
    xfer.resource_id = VIRTIO_GPU_RESOURCE_ID;
    if (gpu_cmd_nodata(&xfer, sizeof(xfer)) < 0)
        return -1;

    memset(&flush, 0, sizeof(flush));
    hdr_init(&flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush.r = xfer.r;
    flush.resource_id = VIRTIO_GPU_RESOURCE_ID;
    if (gpu_cmd_nodata(&flush, sizeof(flush)) < 0)
        return -1;
    return 0;
}

static int virtio_get_mode(display_mode_t *out)
{
    if (!g_ready || !out)
        return -1;
    *out = g_mode;
    return 0;
}

static int virtio_present(const uint32_t *src, uint32_t src_stride_px)
{
    uint32_t y;
    if (!g_ready || !src || !g_fb)
        return -1;

    if (src_stride_px == g_mode.width) {
        memcpy(g_fb, src, g_fb_bytes);
    } else {
        for (y = 0; y < g_mode.height; y++)
            memcpy(g_fb + y * g_mode.width, src + y * src_stride_px,
                   g_mode.width * 4);
    }
    return gpu_transfer_flush(0, 0, g_mode.width, g_mode.height);
}

static int virtio_present_rect(const uint32_t *src, uint32_t src_stride_px,
                               uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row;
    if (!g_ready || !src || !g_fb || w == 0 || h == 0)
        return -1;
    if (x >= g_mode.width || y >= g_mode.height)
        return -1;
    if (x + w > g_mode.width)
        w = g_mode.width - x;
    if (y + h > g_mode.height)
        h = g_mode.height - y;

    for (row = 0; row < h; row++) {
        memcpy(g_fb + (y + row) * g_mode.width + x,
               src + (y + row) * src_stride_px + x,
               w * 4);
    }
    return gpu_transfer_flush(x, y, w, h);
}

static int virtio_gpu_submit(const void *cmd, uint32_t size)
{
    /* 3D/VirGL path reserved — 2D scanout uses present() */
    (void)cmd;
    (void)size;
    return g_ready ? 0 : -1;
}

static int virtio_gpu_bringup(void)
{
    uint32_t width, height;

    if (virtio_pci_init(&g_vd, &g_pci) < 0)
        return -1;
    if (virtio_pci_setup_queue(&g_vd, VIRTIO_GPU_QUEUE_CONTROL, 64) < 0) {
        vga_print("virtio-gpu: controlq failed\n");
        return -1;
    }

    virtio_pci_set_status(&g_vd,
                          (uint8_t)(virtio_pci_get_status(&g_vd) | VIRTIO_STATUS_DRIVER_OK));

    if (gpu_get_display_size(&width, &height) < 0) {
        width = VIRTIO_GPU_DEFAULT_W;
        height = VIRTIO_GPU_DEFAULT_H;
    }

    g_fb_bytes = width * height * 4u;
    g_fb = (uint32_t *)kmalloc_aligned(g_fb_bytes, 4096);
    if (!g_fb)
        return -1;
    memset(g_fb, 0, g_fb_bytes);

    if (gpu_setup_scanout(width, height) < 0)
        return -1;

    memset(&g_mode, 0, sizeof(g_mode));
    g_mode.addr = (uint8_t *)g_fb;
    g_mode.width = width;
    g_mode.height = height;
    g_mode.bpp = 32;
    g_mode.bytes_per_pixel = 4;
    g_mode.pitch = width * 4;
    g_ready = 1;

    /* Initial black frame */
    if (gpu_transfer_flush(0, 0, width, height) < 0)
        return -1;

    vga_print("virtio-gpu: scanout ready\n");
    return 0;
}

static int virtio_drv_probe(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    if (virtio_pci_probe_gpu(&g_pci) == 0) {
        g_device_present = 1;
        return 0;
    }
    g_device_present = 0;
    return 0; /* soft: allow boot without virtio */
}

static int virtio_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;

    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.name = "virtio_gpu";
    g_ops.get_mode = virtio_get_mode;
    g_ops.present = virtio_present;
    g_ops.present_rect = virtio_present_rect;
    g_ops.gpu_submit = virtio_gpu_submit;

    if (!g_device_present) {
        vga_print("virtio-gpu: not present\n");
        return 0;
    }

    if (virtio_gpu_bringup() < 0) {
        vga_print("virtio-gpu: bringup failed\n");
        g_ready = 0;
        return 0; /* keep BGA usable */
    }

    return display_register(&g_ops, DISPLAY_PRIO_VIRTIO);
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "display_virtio", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_DISPLAY;
    d.flags = 0;
    d.priority = 20;
    d.probe = virtio_drv_probe;
    d.init = virtio_drv_init;

    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("display_virtio", NULL) < 0)
        return -1;
    return 0;
}
