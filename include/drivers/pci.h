#ifndef MYKERNEL_DRIVERS_PCI_H
#define MYKERNEL_DRIVERS_PCI_H

#include <kernel/types.h>

#define PCI_VENDOR_INVALID  0xFFFF

#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_CAPABILITY_LIST 0x34
#define PCI_COMMAND_IO      (1u << 0)
#define PCI_COMMAND_MEMORY  (1u << 1)
#define PCI_COMMAND_MASTER  (1u << 2)
#define PCI_STATUS_CAP_LIST (1u << 4)
#define PCI_CAP_ID_VNDR     0x09

typedef struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint32_t bar[6];
} pci_device_t;

typedef int (*pci_enum_fn)(const pci_device_t *dev, void *ctx);

void     pci_init(void);
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint32_t pci_bar_addr(uint32_t bar);
uint32_t pci_bar_phys(const pci_device_t *dev, int bar_index);
int      pci_enable_bus_master(const pci_device_t *dev);
int      pci_find(uint16_t vendor, uint16_t device, pci_device_t *out);
int      pci_find_class(uint8_t class_code, uint8_t subclass, pci_device_t *out);
int      pci_enumerate(pci_enum_fn fn, void *ctx);

#endif
