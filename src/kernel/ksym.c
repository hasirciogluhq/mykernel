#include <kernel/ksym.h>
#include <kernel/string.h>
#include <kernel/heap.h>
#include <kernel/module.h>
#include <kernel/mkdx_api.h>
#include <drivers/driver.h>
#include <drivers/pci.h>
#include <drivers/display.h>
#include <drivers/vga.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>
#include <drivers/ps2.h>

static const ksym_t g_ksyms[] = {
    { "kmalloc",            (void *)kmalloc },
    { "kmalloc_aligned",    (void *)kmalloc_aligned },
    { "kfree",              (void *)kfree },
    { "heap_used",          (void *)heap_used },
    { "heap_free",          (void *)heap_free },

    { "memcpy",             (void *)memcpy },
    { "memset",             (void *)memset },
    { "memmove",            (void *)memmove },
    { "strlen",             (void *)strlen },
    { "strcmp",             (void *)strcmp },
    { "strncmp",            (void *)strncmp },
    { "strcpy",             (void *)strcpy },
    { "strncpy",            (void *)strncpy },

    { "driver_register",    (void *)driver_register },
    { "driver_unregister",  (void *)driver_unregister },
    { "driver_load",        (void *)driver_load },
    { "driver_unload",      (void *)driver_unload },
    { "driver_find",        (void *)driver_find },
    { "driver_get_state",   (void *)driver_get_state },
    { "drivers_poll",       (void *)drivers_poll },

    { "pci_init",           (void *)pci_init },
    { "pci_config_read",    (void *)pci_config_read },
    { "pci_config_write",   (void *)pci_config_write },
    { "pci_config_read8",   (void *)pci_config_read8 },
    { "pci_config_read16",  (void *)pci_config_read16 },
    { "pci_config_write16", (void *)pci_config_write16 },
    { "pci_bar_addr",       (void *)pci_bar_addr },
    { "pci_bar_phys",       (void *)pci_bar_phys },
    { "pci_enable_bus_master", (void *)pci_enable_bus_master },
    { "pci_find",           (void *)pci_find },
    { "pci_find_class",     (void *)pci_find_class },
    { "pci_enumerate",      (void *)pci_enumerate },

    { "display_register",   (void *)display_register },
    { "display_unregister", (void *)display_unregister },
    { "display_active",     (void *)display_active },
    { "display_get_screen_size", (void *)display_get_screen_size },

    { "mkdx_api_register",  (void *)mkdx_api_register },
    { "mkdx_api_get",       (void *)mkdx_api_get },

    { "vga_print",          (void *)vga_print },
    { "vga_putc",           (void *)vga_putc },

    { "mouse_get",          (void *)mouse_get },
    { "mouse_pop_event",    (void *)mouse_pop_event },
    { "mouse_set_bounds",   (void *)mouse_set_bounds },
    { "keyboard_getchar",   (void *)keyboard_getchar },
    { "keyboard_modifiers", (void *)keyboard_modifiers },
    { "ps2_poll",           (void *)ps2_poll },

    { NULL, NULL }
};

void ksym_init(void)
{
}

void *ksym_lookup(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; g_ksyms[i].name; i++) {
        if (strcmp(g_ksyms[i].name, name) == 0)
            return g_ksyms[i].addr;
    }
    return NULL;
}
