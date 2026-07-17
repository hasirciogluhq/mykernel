#ifndef MYKERNEL_DRIVERS_DRIVER_H
#define MYKERNEL_DRIVERS_DRIVER_H

#include <kernel/types.h>

#define DRIVER_MAX          64
#define DRIVER_NAME_MAX     32
#define DRIVER_VERSION_MAX  16

typedef enum {
    DRIVER_KIND_INTERNAL = 0, /* built into the kernel image */
    DRIVER_KIND_CUSTOM   = 1  /* registered at runtime */
} driver_kind_t;

typedef enum {
    DRIVER_STATE_NONE = 0,
    DRIVER_STATE_REGISTERED,
    DRIVER_STATE_LOADED,
    DRIVER_STATE_FAILED,
    DRIVER_STATE_UNLOADED
} driver_state_t;

typedef enum {
    DRIVER_CLASS_MISC = 0,
    DRIVER_CLASS_DISPLAY,
    DRIVER_CLASS_INPUT,
    DRIVER_CLASS_BUS,
    DRIVER_CLASS_CHAR,
    DRIVER_CLASS_BLOCK,
    DRIVER_CLASS_FS
} driver_class_t;

#define DRIVER_FLAG_AUTO  (1u << 0) /* load via drivers_load_all() */
#define DRIVER_FLAG_POLL  (1u << 1) /* participate in drivers_poll() */
#define DRIVER_FLAG_EARLY (1u << 2) /* already running before framework */

typedef struct driver driver_t;

struct driver {
    char           name[DRIVER_NAME_MAX];
    char           version[DRIVER_VERSION_MAX];
    driver_kind_t  kind;
    driver_class_t class;
    uint32_t       flags;
    int            priority; /* lower loads first */

    int  (*probe)(driver_t *drv, void *ctx);
    int  (*init)(driver_t *drv, void *ctx);
    void (*exit)(driver_t *drv);
    void (*poll)(driver_t *drv);

    void *priv;
};

typedef struct driver_slot {
    driver_t       drv;
    driver_state_t state;
    int            used;
} driver_slot_t;

typedef struct driver_info {
    char           name[DRIVER_NAME_MAX];
    char           version[DRIVER_VERSION_MAX];
    driver_kind_t  kind;
    driver_class_t class;
    driver_state_t state;
    uint32_t       flags;
    int            priority;
} driver_info_t;

/* Framework lifecycle */
void driver_framework_init(void);
void drivers_register_internal(void);

/* Register / unregister (internal or custom) */
int  driver_register(const driver_t *drv);
int  driver_unregister(const char *name);

/*
 * Attach an already-running driver (e.g. early VGA) without calling init.
 * Driver must already be registered.
 */
int  driver_attach(const char *name);

/* Load / unload by name */
int  driver_load(const char *name, void *ctx);
int  driver_unload(const char *name);

/* Bulk helpers */
int  drivers_load_all(void *ctx); /* AUTO-flagged, priority order */
void drivers_poll(void);          /* poll all LOADED + POLL drivers */

/* Lookup / inventory */
driver_t       *driver_find(const char *name);
driver_state_t  driver_get_state(const char *name);
size_t          driver_count(void);
size_t          driver_loaded_count(void);
const driver_slot_t *driver_table(size_t *count);
int             driver_list(driver_info_t *out, size_t max, size_t *written);

#endif
