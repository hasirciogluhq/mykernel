#include <drivers/driver.h>
#include <arch/x86/cpu.h>
#include <kernel/string.h>

static driver_slot_t g_slots[DRIVER_MAX];
static size_t        g_count;
static int           g_ready;

static driver_slot_t *find_slot(const char *name)
{
    if (!name || !name[0])
        return NULL;

    for (size_t i = 0; i < DRIVER_MAX; i++) {
        if (!g_slots[i].used)
            continue;
        if (strcmp(g_slots[i].drv.name, name) == 0)
            return &g_slots[i];
    }
    return NULL;
}

static driver_slot_t *alloc_slot(void)
{
    for (size_t i = 0; i < DRIVER_MAX; i++) {
        if (!g_slots[i].used)
            return &g_slots[i];
    }
    return NULL;
}

void driver_framework_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_count = 0;
    g_ready = 1;
}

int driver_register(const driver_t *drv)
{
    if (!g_ready || !drv || !drv->name[0])
        return -1;
    if (find_slot(drv->name))
        return -1;

    driver_slot_t *slot = alloc_slot();
    if (!slot)
        return -1;

    memset(slot, 0, sizeof(*slot));
    slot->drv = *drv;
    slot->drv.name[DRIVER_NAME_MAX - 1] = '\0';
    slot->drv.version[DRIVER_VERSION_MAX - 1] = '\0';
    slot->state = DRIVER_STATE_REGISTERED;
    slot->used = 1;
    g_count++;
    return 0;
}

int driver_unregister(const char *name)
{
    driver_slot_t *slot = find_slot(name);
    if (!slot)
        return -1;
    if (slot->state == DRIVER_STATE_LOADED)
        return -1; /* must unload first */

    memset(slot, 0, sizeof(*slot));
    if (g_count > 0)
        g_count--;
    return 0;
}

int driver_attach(const char *name)
{
    driver_slot_t *slot = find_slot(name);
    if (!slot)
        return -1;
    if (slot->state == DRIVER_STATE_LOADED)
        return 0;

    slot->state = DRIVER_STATE_LOADED;
    return 0;
}

int driver_load(const char *name, void *ctx)
{
    driver_slot_t *slot = find_slot(name);
    if (!slot)
        return -1;

    if (slot->state == DRIVER_STATE_LOADED)
        return 0;

    driver_t *drv = &slot->drv;

    if (drv->probe) {
        int pr = drv->probe(drv, ctx);
        if (pr < 0) {
            slot->state = DRIVER_STATE_FAILED;
            return -1;
        }
    }

    if (drv->init) {
        int rc = drv->init(drv, ctx);
        if (rc < 0) {
            slot->state = DRIVER_STATE_FAILED;
            return -1;
        }
    }

    slot->state = DRIVER_STATE_LOADED;
    return 0;
}

int driver_unload(const char *name)
{
    driver_slot_t *slot = find_slot(name);
    if (!slot)
        return -1;
    if (slot->state != DRIVER_STATE_LOADED)
        return -1;

    driver_t *drv = &slot->drv;
    if (drv->exit)
        drv->exit(drv);

    slot->state = DRIVER_STATE_UNLOADED;
    return 0;
}

static void sort_auto_indices(size_t *idx, size_t n)
{
    /* insertion sort by priority (ascending) */
    for (size_t i = 1; i < n; i++) {
        size_t key = idx[i];
        int prio = g_slots[key].drv.priority;
        size_t j = i;
        while (j > 0 && g_slots[idx[j - 1]].drv.priority > prio) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

int drivers_load_all(void *ctx)
{
    if (!g_ready)
        return -1;

    size_t idx[DRIVER_MAX];
    size_t n = 0;

    for (size_t i = 0; i < DRIVER_MAX; i++) {
        if (!g_slots[i].used)
            continue;
        if (!(g_slots[i].drv.flags & DRIVER_FLAG_AUTO))
            continue;
        if (g_slots[i].state == DRIVER_STATE_LOADED)
            continue;
        idx[n++] = i;
    }

    sort_auto_indices(idx, n);

    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        driver_slot_t *slot = &g_slots[idx[i]];
        if (driver_load(slot->drv.name, ctx) < 0)
            failed++;
    }
    return failed ? -1 : 0;
}

void drivers_poll(void)
{
    /* Pollable drivers (PS/2, mkdx, …) are not SMP-safe — BSP only. */
    if (cpu_id() != 0)
        return;

    for (size_t i = 0; i < DRIVER_MAX; i++) {
        if (!g_slots[i].used)
            continue;
        if (g_slots[i].state != DRIVER_STATE_LOADED)
            continue;
        if (!(g_slots[i].drv.flags & DRIVER_FLAG_POLL))
            continue;
        if (g_slots[i].drv.poll)
            g_slots[i].drv.poll(&g_slots[i].drv);
    }
}

driver_t *driver_find(const char *name)
{
    driver_slot_t *slot = find_slot(name);
    return slot ? &slot->drv : NULL;
}

driver_state_t driver_get_state(const char *name)
{
    driver_slot_t *slot = find_slot(name);
    return slot ? slot->state : DRIVER_STATE_NONE;
}

size_t driver_count(void)
{
    return g_count;
}

size_t driver_loaded_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < DRIVER_MAX; i++) {
        if (g_slots[i].used && g_slots[i].state == DRIVER_STATE_LOADED)
            n++;
    }
    return n;
}

const driver_slot_t *driver_table(size_t *count)
{
    if (count)
        *count = DRIVER_MAX;
    return g_slots;
}

int driver_list(driver_info_t *out, size_t max, size_t *written)
{
    size_t n = 0;

    for (size_t i = 0; i < DRIVER_MAX && n < max; i++) {
        if (!g_slots[i].used)
            continue;

        if (out) {
            driver_info_t *info = &out[n];
            memset(info, 0, sizeof(*info));
            strncpy(info->name, g_slots[i].drv.name, DRIVER_NAME_MAX - 1);
            strncpy(info->version, g_slots[i].drv.version, DRIVER_VERSION_MAX - 1);
            info->kind = g_slots[i].drv.kind;
            info->class = g_slots[i].drv.class;
            info->state = g_slots[i].state;
            info->flags = g_slots[i].drv.flags;
            info->priority = g_slots[i].drv.priority;
        }
        n++;
    }

    if (written)
        *written = n;
    return 0;
}
