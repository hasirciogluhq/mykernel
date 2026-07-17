#ifndef MYKERNEL_VIRTIO_PCI_H
#define MYKERNEL_VIRTIO_PCI_H

#include <drivers/pci.h>
#include <kernel/types.h>

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       0x80

#define VIRTIO_F_VERSION_1         32

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VIRTIO_GPU_QUEUE_CONTROL 0

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

typedef struct virtio_pci_dev {
    pci_device_t pci;
    volatile uint8_t *common;
    volatile uint8_t *notify;
    volatile uint8_t *isr;
    volatile uint8_t *device_cfg;
    uint32_t notify_off_multiplier;
    uint16_t queue_notify_off;

    uint16_t qsize;
    virtq_desc_t *desc;
    virtq_avail_t *avail;
    virtq_used_t *used;
    void *queue_mem;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used_idx;
} virtio_pci_dev_t;

int  virtio_pci_probe_gpu(pci_device_t *out);
int  virtio_pci_init(virtio_pci_dev_t *vd, const pci_device_t *pci);
int  virtio_pci_setup_queue(virtio_pci_dev_t *vd, uint16_t qindex, uint16_t max_size);
void virtio_pci_set_status(virtio_pci_dev_t *vd, uint8_t status);
uint8_t virtio_pci_get_status(virtio_pci_dev_t *vd);
int  virtio_pci_negotiate_features(virtio_pci_dev_t *vd);
int  virtio_pci_queue_submit(virtio_pci_dev_t *vd,
                             const void *out, uint32_t out_len,
                             void *in, uint32_t in_len);

#endif
