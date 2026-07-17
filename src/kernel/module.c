#include <kernel/module.h>
#include <kernel/ksym.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/initrd.h>
#include <drivers/vga.h>

#define EI_NIDENT 16
#define ET_REL    1
#define EM_386    3
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_REL    9
#define SHN_UNDEF  0
#define SHN_ABS    0xFFF1
#define SHN_COMMON 0xFFF2
#define SHF_ALLOC  0x2
#define STB_LOCAL  0
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2
#define STT_SECTION 3
#define STT_FILE   4
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define R_386_NONE 0
#define R_386_32   1
#define R_386_PC32 2

#define MODULE_MAX     48
#define MODULE_MAX_SH  64

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

static kmod_t g_mods[MODULE_MAX];
static size_t g_mod_count;

static int elf_ok(const Elf32_Ehdr *eh, size_t size)
{
    if (size < sizeof(Elf32_Ehdr))
        return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 1) /* ELFCLASS32 */
        return 0;
    if (eh->e_type != ET_REL || eh->e_machine != EM_386)
        return 0;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr))
        return 0;
    if ((size_t)eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf32_Shdr) > size)
        return 0;
    return 1;
}

static uintptr_t align_up(uintptr_t v, uint32_t a)
{
    if (a <= 1)
        return v;
    return (v + (a - 1)) & ~(uintptr_t)(a - 1);
}

static void *sym_addr(Elf32_Sym *sym, uint8_t **sec_base, Elf32_Shdr *shdrs)
{
    if (sym->st_shndx == SHN_UNDEF)
        return NULL;
    if (sym->st_shndx == SHN_ABS)
        return (void *)(uintptr_t)sym->st_value;
    if (sym->st_shndx == SHN_COMMON)
        return NULL;
    if (!sec_base[sym->st_shndx])
        return NULL;
    (void)shdrs;
    return sec_base[sym->st_shndx] + sym->st_value;
}

static const char *sym_name(Elf32_Sym *sym, const char *strtab)
{
    if (!strtab || !sym->st_name)
        return "";
    return strtab + sym->st_name;
}

int modules_load_blob(const char *name, const void *data, size_t size)
{
    const Elf32_Ehdr *eh;
    Elf32_Shdr *shdrs;
    const char *shstr;
    uint8_t *sec_base[MODULE_MAX_SH];
    Elf32_Sym *symtab = NULL;
    const char *strtab = NULL;
    uint32_t nsyms = 0;
    uint16_t i, j;
    size_t image_size = 0;
    uint8_t *image = NULL;
    uintptr_t cursor;
    kmod_init_fn entry = NULL;
    kmod_t *slot;

    if (!data || size == 0 || g_mod_count >= MODULE_MAX)
        return -1;

    eh = (const Elf32_Ehdr *)data;
    if (!elf_ok(eh, size)) {
        vga_print("kmod: bad ELF\n");
        return -1;
    }
    if (eh->e_shnum > MODULE_MAX_SH) {
        vga_print("kmod: too many sections\n");
        return -1;
    }

    shdrs = (Elf32_Shdr *)((const uint8_t *)data + eh->e_shoff);
    shstr = (const char *)data + shdrs[eh->e_shstrndx].sh_offset;
    memset(sec_base, 0, sizeof(sec_base));

    /* Size allocable image */
    for (i = 0; i < eh->e_shnum; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC))
            continue;
        image_size = (size_t)align_up((uintptr_t)image_size, shdrs[i].sh_addralign);
        image_size += shdrs[i].sh_size;
    }

    image = (uint8_t *)kmalloc_aligned(image_size ? image_size : 1, 16);
    if (!image)
        return -1;
    memset(image, 0, image_size);

    cursor = (uintptr_t)image;
    for (i = 0; i < eh->e_shnum; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC))
            continue;
        cursor = align_up(cursor, shdrs[i].sh_addralign);
        sec_base[i] = (uint8_t *)cursor;
        if (shdrs[i].sh_type != 8 /* SHT_NOBITS */) {
            if (shdrs[i].sh_offset + shdrs[i].sh_size > size) {
                kfree(image);
                return -1;
            }
            memcpy(sec_base[i], (const uint8_t *)data + shdrs[i].sh_offset,
                   shdrs[i].sh_size);
        }
        cursor += shdrs[i].sh_size;
    }

    /* Find symtab */
    for (i = 0; i < eh->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab = (Elf32_Sym *)((const uint8_t *)data + shdrs[i].sh_offset);
            nsyms = shdrs[i].sh_size / sizeof(Elf32_Sym);
            strtab = (const char *)data + shdrs[shdrs[i].sh_link].sh_offset;
            break;
        }
    }
    if (!symtab) {
        vga_print("kmod: no symtab\n");
        kfree(image);
        return -1;
    }

    /* Apply relocations */
    for (i = 0; i < eh->e_shnum; i++) {
        Elf32_Rel *rels;
        uint32_t nrels;
        uint16_t target;

        if (shdrs[i].sh_type != SHT_REL)
            continue;

        target = (uint16_t)shdrs[i].sh_info;
        if (!sec_base[target])
            continue;

        rels = (Elf32_Rel *)((const uint8_t *)data + shdrs[i].sh_offset);
        nrels = shdrs[i].sh_size / sizeof(Elf32_Rel);

        for (j = 0; j < nrels; j++) {
            uint32_t sym_idx = ELF32_R_SYM(rels[j].r_info);
            uint32_t type = ELF32_R_TYPE(rels[j].r_info);
            Elf32_Sym *sym = &symtab[sym_idx];
            uint32_t *loc = (uint32_t *)(sec_base[target] + rels[j].r_offset);
            void *S = NULL;
            uint32_t A;
            uintptr_t P;

            if (sym->st_shndx == SHN_UNDEF) {
                const char *nm = sym_name(sym, strtab);
                S = ksym_lookup(nm);
                if (!S) {
                    vga_print("kmod: unresolved ");
                    vga_print(nm);
                    vga_print("\n");
                    kfree(image);
                    return -1;
                }
            } else {
                S = sym_addr(sym, sec_base, shdrs);
                if (!S && ELF32_ST_TYPE(sym->st_info) != STT_NOTYPE) {
                    vga_print("kmod: bad local sym\n");
                    kfree(image);
                    return -1;
                }
            }

            A = *loc;
            P = (uintptr_t)loc;

            switch (type) {
            case R_386_NONE:
                break;
            case R_386_32:
                *loc = (uint32_t)(uintptr_t)S + A;
                break;
            case R_386_PC32:
                *loc = (uint32_t)((uintptr_t)S + A - P);
                break;
            default:
                vga_print("kmod: bad reloc type\n");
                kfree(image);
                return -1;
            }
        }
    }

    /* Find kmod_init */
    for (i = 0; i < nsyms; i++) {
        const char *nm = sym_name(&symtab[i], strtab);
        if (strcmp(nm, "kmod_init") == 0) {
            entry = (kmod_init_fn)sym_addr(&symtab[i], sec_base, shdrs);
            break;
        }
    }
    if (!entry) {
        vga_print("kmod: no kmod_init\n");
        kfree(image);
        return -1;
    }

    slot = &g_mods[g_mod_count];
    memset(slot, 0, sizeof(*slot));
    if (name)
        strncpy(slot->name, name, KMOD_NAME_MAX - 1);
    else if (shstr)
        strncpy(slot->name, "kmod", KMOD_NAME_MAX - 1);
    slot->base = image;
    slot->size = image_size;
    slot->init = entry;

    if (entry() < 0) {
        vga_print("kmod: init failed\n");
        kfree(image);
        return -1;
    }

    slot->loaded = 1;
    g_mod_count++;
    (void)shstr;
    return 0;
}

static const char *basename_of(const char *path)
{
    const char *p;
    if (!path)
        return "kmod";
    p = path;
    while (*path) {
        if (*path == '/')
            p = path + 1;
        path++;
    }
    return p;
}

int modules_load_initrd(const void *data, size_t size)
{
    const initrd_header_t *hdr;
    uint32_t i;
    size_t table_bytes;

    if (!data || size < sizeof(uint32_t) * 2)
        return -1;

    hdr = (const initrd_header_t *)data;
    if (hdr->magic != INITRD_MAGIC || hdr->count == 0 ||
        hdr->count > INITRD_MAX_FILES) {
        return -1;
    }

    table_bytes = sizeof(uint32_t) * 2 + (size_t)hdr->count * sizeof(initrd_file_t);
    if (size < table_bytes)
        return -1;

    for (i = 0; i < hdr->count; i++) {
        const initrd_file_t *f = &hdr->files[i];
        const uint8_t *blob;
        uint32_t magic;

        if (f->size == 0 || f->offset + f->size > size)
            return -1;
        blob = (const uint8_t *)data + f->offset;

        /* .mke apps are spawned later by mke_spawn_from_* */
        if (f->size >= 4) {
            magic = blob[0] | ((uint32_t)blob[1] << 8) |
                    ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
            if (magic == 0x31454B4Du) /* MKE1 */
                continue;
        }

        vga_print("load ");
        vga_print(f->name);
        vga_print("\n");
        if (modules_load_blob(f->name, blob, f->size) < 0) {
            /* Soft-fail one kmod so BGA/MKDX can still boot without virtio. */
            vga_print("kmod load skipped: ");
            vga_print(f->name);
            vga_print("\n");
            continue;
        }
    }
    return 0;
}

int modules_load_from_mbi(multiboot_info_t *mbi)
{
    multiboot_mod_list_t *mods;
    uint32_t i;

    if (!mbi || !(mbi->flags & MULTIBOOT_INFO_MODS) || mbi->mods_count == 0) {
        vga_print("no multiboot modules\n");
        return -1;
    }

    mods = (multiboot_mod_list_t *)(uintptr_t)mbi->mods_addr;
    for (i = 0; i < mbi->mods_count; i++) {
        const void *start = (const void *)(uintptr_t)mods[i].mod_start;
        size_t sz = (size_t)(mods[i].mod_end - mods[i].mod_start);
        const char *cmd = mods[i].cmdline
                              ? (const char *)(uintptr_t)mods[i].cmdline
                              : NULL;
        const initrd_header_t *hdr;

        if (sz < 4)
            return -1;

        hdr = (const initrd_header_t *)start;
        if (hdr->magic == INITRD_MAGIC) {
            if (modules_load_initrd(start, sz) < 0)
                return -1;
            continue;
        }

        /* Raw .kmod ELF */
        vga_print("load ");
        vga_print(basename_of(cmd));
        vga_print("\n");
        if (modules_load_blob(basename_of(cmd), start, sz) < 0)
            return -1;
    }
    return 0;
}

kmod_t *module_find(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < g_mod_count; i++) {
        if (strcmp(g_mods[i].name, name) == 0)
            return &g_mods[i];
    }
    return NULL;
}
