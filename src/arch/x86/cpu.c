#include <arch/x86/cpu.h>
#include <arch/x86/lapic.h>
#include <kernel/string.h>

static cpu_t g_cpus[CPU_MAX];
static int   g_cpu_count;
static int   g_bsp_ready;

void cpu_init_bsp(void)
{
    uint8_t apic;

    memset(g_cpus, 0, sizeof(g_cpus));
    g_cpu_count = 0;
    g_bsp_ready = 0;

    lapic_init_bsp();
    apic = (uint8_t)lapic_id();

    g_cpus[0].id = 0;
    g_cpus[0].apic_id = apic;
    g_cpus[0].online = 1;
    g_cpus[0].package = 0;
    g_cpus[0].core = 0;
    g_cpus[0].smt = 0;
    g_cpus[0].started = 1;
    g_cpus[0].current = NULL;
    g_cpus[0].idle = NULL;
    g_cpu_count = 1;
    g_bsp_ready = 1;
}

cpu_t *cpu_alloc(uint8_t apic_id)
{
    cpu_t *c;

    if (g_cpu_count >= CPU_MAX)
        return NULL;
    c = &g_cpus[g_cpu_count];
    memset(c, 0, sizeof(*c));
    c->id = g_cpu_count;
    c->apic_id = apic_id;
    /* Flat APIC probe: one package, core == dense id, no SMT map yet. */
    c->package = 0;
    c->core = c->id;
    c->smt = 0;
    g_cpu_count++;
    return c;
}

void cpu_rollback_last(void)
{
    if (g_cpu_count <= 1)
        return;
    g_cpu_count--;
    memset(&g_cpus[g_cpu_count], 0, sizeof(g_cpus[0]));
}

cpu_t *cpu_get(int id)
{
    if (id < 0 || id >= g_cpu_count)
        return NULL;
    return &g_cpus[id];
}

cpu_t *cpu_by_apic(uint8_t apic_id)
{
    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id == apic_id)
            return &g_cpus[i];
    }
    return NULL;
}

int cpu_count(void)
{
    return g_cpu_count;
}

int cpu_id(void)
{
    cpu_t *c;

    if (!g_bsp_ready)
        return 0;
    c = cpu_by_apic((uint8_t)lapic_id());
    return c ? c->id : 0;
}

cpu_t *cpu_current(void)
{
    cpu_t *c;

    if (!g_bsp_ready)
        return &g_cpus[0];
    c = cpu_by_apic((uint8_t)lapic_id());
    return c ? c : &g_cpus[0];
}
