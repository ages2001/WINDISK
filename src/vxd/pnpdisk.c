/* pnpdisk.c - Windows 95/98/ME Plug and Play, the WINDISK part. WINDISK.VXD is the DevLoader
   of the disk controllers (Hard disk controllers, WINDISK.INF): PCI IDE, SATA AHCI (and the
   AHCI-compatible RAID mode), NVMe and SD host controllers, the latter also as ACPI devices.
   Every disk and CD/DVD drive on them becomes a child devnode that the I/O Supervisor loads
   with WINDISK.PDR as its port driver (Disk drives, WINDISKD.INF; CD-ROM, WINDISKC.INF). */
#include "pnpint.h"

int pkg_enumerates(void)
{
    return 1;
}

/* ACPI SD host controllers (Bay Trail, Cherry Trail, Broxton / Apollo Lake, the generic
   SDHCI id); the Configuration Manager writes them with or without the '*' */
static const char *const acpi_sd[] = {
    "80860F14", "80860F16", "INT33BB", "80865ACA", "80865AD0", "PNP0D40"
};

static int acpi_sd_id(const char *id)
{
    int i;
    if (strncmp(id, "ACPI\\", 5)) return 0;
    id += 5;
    if (*id == '*') id++;
    for (i = 0; i < (int)ARRAY_SIZE(acpi_sd); i++) {
        u32 n = strlen(acpi_sd[i]);
        if (!strncmp(id, acpi_sd[i], n) && (id[n] == '\\' || id[n] == 0)) return 1;
    }
    return 0;
}

int pkg_devnode_kind(const char *id)
{
    return acpi_sd_id(id) ? PK_LEGACY : 0;
}

static const struct { u16 vendor; const char *name; } vendors[] = {
    { 0x8086, "Intel" }, { 0x1022, "AMD" }, { 0x1002, "AMD" }, { 0x10DE, "NVIDIA" }, { 0x1106, "VIA" },
    { 0x1039, "SiS" }, { 0x10B9, "ALi" }, { 0x1095, "Silicon Image" }, { 0x105A, "Promise" },
    { 0x197B, "JMicron" }, { 0x1B4B, "Marvell" }, { 0x1B21, "ASMedia" }, { 0x1103, "HighPoint" },
    { 0x1283, "ITE" }, { 0x1166, "ServerWorks" }, { 0x1045, "OPTi" }, { 0x1078, "Cyrix" },
    { 0x144D, "Samsung" }, { 0x15B7, "SanDisk / WD" }, { 0x1C5C, "SK hynix" }, { 0x1987, "Phison" },
    { 0x126F, "Silicon Motion" }, { 0x1179, "Toshiba" }, { 0x1E0F, "KIOXIA" }, { 0x2646, "Kingston" },
    { 0x1344, "Micron" }, { 0x1CC1, "ADATA" }, { 0x1D97, "Shenzhen Longsys" }, { 0x10EC, "Realtek" },
    { 0x1217, "O2 Micro" }, { 0x1180, "Ricoh" }, { 0x104C, "Texas Instruments" }, { 0x1524, "ENE" },
    { 0x8080, "Generic" }, { 0x1B36, "QEMU" }, { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" },
    { 0x1AF4, "Virtio" }, { 0x17F3, "RDC" }, { 0x1D17, "Zhaoxin" },
};

static const char *vendor_name(u32 vid, char *tmp, u32 max)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(vendors); i++)
        if (vendors[i].vendor == vid) return vendors[i].name;
    ksnprintf(tmp, max, "PCI %04lX", vid);
    return tmp;
}

/* Intel SATA controllers by chip set (AHCI and RAID device ids) */
static const struct { u16 lo, hi; const char *name; } intel_sata[] = {
    { 0x2652, 0x2653, "ICH6" }, { 0x27C1, 0x27C6, "ICH7" }, { 0x2821, 0x282A, "ICH8" },
    { 0x2922, 0x292F, "ICH9" }, { 0x3A02, 0x3A2F, "ICH10" }, { 0x3B22, 0x3B2F, "5 Series" },
    { 0x1C02, 0x1C0F, "6 Series" }, { 0x1E02, 0x1E0F, "7 Series" }, { 0x8C02, 0x8C0F, "8 Series" },
    { 0x9C02, 0x9C0F, "8 Series mobile" }, { 0x8C82, 0x8C8F, "9 Series" }, { 0xA102, 0xA10F, "100 Series" },
    { 0xA282, 0xA28F, "200 Series" }, { 0xA352, 0xA35F, "300 Series" }, { 0x0F20, 0x0F23, "Bay Trail" },
    { 0x22A3, 0x22A3, "Braswell" }, { 0x5AE3, 0x5AE3, "Apollo Lake" }, { 0x9D03, 0x9D07, "Sunrise Point" },
    { 0x06D2, 0x06D7, "400 Series" }, { 0x43D2, 0x43D7, "500 Series" }, { 0x7AE2, 0x7AE2, "600 Series" },
};

/* Device Manager name of a controller from its PCI class code: Windows 95 matches only the
   4-digit PCI\CC_xxxx IDs, so the generic INF entry gets the precise name here */
const char *pkg_desc(u32 bdf)
{
    static char buf[112];
    int b = (int)(bdf >> 8), d = (int)((bdf >> 3) & 0x1F), f = (int)(bdf & 7), i;
    u32 cls = pci_rd32(b, d, f, 8) >> 8, id = pci_rd32(b, d, f, 0);
    u16 vid = (u16)id, dev = (u16)(id >> 16);
    u8 prog = (u8)cls;
    char tmp[16];
    const char *v = vendor_name(vid, tmp, sizeof(tmp)), *chip = "";
    if (vid == 0x8086)
        for (i = 0; i < (int)ARRAY_SIZE(intel_sata); i++)
            if (dev >= intel_sata[i].lo && dev <= intel_sata[i].hi) chip = intel_sata[i].name;
    switch (cls >> 8) {
    case 0x0101: {
        const char *mode;
        if ((prog & 5) == 5) mode = "native mode";
        else if (prog & 5) mode = "primary/secondary channels, mixed mode";
        else mode = "primary/secondary channels";
        ksnprintf(buf, sizeof(buf), "WINDISK %s%s%s PCI IDE Controller (%s%s)", v, chip[0] ? " " : "", chip, mode,
                  (prog & 0x80) ? ", bus master" : "");
        return buf;
    }
    case 0x0106:
        ksnprintf(buf, sizeof(buf), "WINDISK %s%s%s SATA AHCI Controller", v, chip[0] ? " " : "", chip);
        return buf;
    case 0x0104:
        ksnprintf(buf, sizeof(buf), "WINDISK %s%s%s SATA Controller (RAID mode, as AHCI)", v, chip[0] ? " " : "", chip);
        return buf;
    case 0x0108:
        ksnprintf(buf, sizeof(buf), "WINDISK %s NVMe (NVM Express) Controller", v);
        return buf;
    case 0x0805:
        ksnprintf(buf, sizeof(buf), "WINDISK %s SD Host Controller (SD/SDHC/SDXC)", v);
        return buf;
    }
    return NULL;
}

/* ---- ACPI SD host controllers (PK_LEGACY): started by us with the memory window the
   Configuration Manager allocated ---- */
static u32 next_acpi_key = 0xF000;

void pkg_child_config(struct pnode *p, u32 func)
{
    struct cmconfig cfg;
    if (p->kind != PK_LEGACY) return;
    if (func == CONFIG_START && p->started != 2) {
        int r;
        memset(&cfg, 0, sizeof(cfg));
        if (cm_get_alloc_log_conf(&cfg, p->dn, 0) != CR_SUCCESS || !cfg.nmem) {
            kprintf("pnp: %s: no memory window\n", p->id);
            return;
        }
        if (!p->bdf) p->bdf = next_acpi_key++;
        pnp_set_desc(p->dn, "WINDISK SD Host Controller (SD/eMMC, ACPI)");
        if (!vxd_core_up()) vxd_core_start_pnp();
        if (!vxd_core_up()) return;
        vmm_begin_crit();
        r = core_pnp_start_mmio(p->bdf, cfg.membase[0], cfg.memlen[0], cfg.nirq ? cfg.irq[0] : -1);
        vmm_end_crit();
        kprintf("pnp: %s: SD host at %08lx, %d slot(s)\n", p->id, cfg.membase[0], r);
        p->started = 2;
    } else if (func == CONFIG_STOP || func == CONFIG_REMOVE || func == CONFIG_PREREMOVE) {
        if (p->started == 2) {
            vmm_begin_crit();
            core_pnp_stop_mmio(p->bdf);
            vmm_end_crit();
        }
        p->started = 0;
    }
}

/* ---- disks and CD/DVD drives: one child devnode each ---- */
struct diskview {
    u32 key, key2, parent_bdf;
    u8 kind, cdrom, chan, legacy;
    char model[41];
};

static struct pnode *parent_of(const struct diskview *v)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        if (!p->dn || p->bdf != v->parent_bdf || p->started != 2) continue;
        if (p->kind == PK_PCI || p->kind == PK_LEGACY) return p;
    }
    return NULL;
}

/* model string as part of a hardware id: letters, digits, '-', '.', the rest '_' */
static void id_model(char *out, const char *m, u32 max)
{
    u32 n = 0;
    for (; *m && n + 1 < max; m++) {
        char c = *m;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            c = '_';
        out[n++] = c;
    }
    out[n] = 0;
}

static void create_child(const struct diskview *v)
{
    static const char *const bus[] = { "?", "AHCI", "NVME", "USB", "IDE", "SD", "MMC" };
    struct pnode *parent = parent_of(v), *p;
    char id[96], inst[32], hw[160], mdl[44], desc[64];
    const char *what = v->cdrom ? "CDROM" : "DISK", *b = v->kind < 7 ? bus[v->kind] : "?";
    u32 pb = v->parent_bdf;
    if (!parent) return;
    ksnprintf(inst, sizeof(inst), "B%02lXD%02lXF%lX", pb >> 8, (pb >> 3) & 0x1F, pb & 7);
    switch (v->kind) {
    case DK_IDE:
        ksnprintf(id, sizeof(id), "C%u%c", v->chan, (v->key & 1) ? 'S' : 'M');
        break;
    case DK_AHCI:
        ksnprintf(id, sizeof(id), "P%lu", v->key & 0xFF);
        break;
    case DK_NVME:
        ksnprintf(id, sizeof(id), "N%lu", v->key & 0x7FFF);
        break;
    default:
        ksnprintf(id, sizeof(id), "S%08lX", v->key2);     /* the card: slots share the key */
        break;
    }
    strlcat(inst, id, sizeof(inst));
    ksnprintf(id, sizeof(id), "WINDISK\\%s&%s\\%s", b, what, inst);
    id_model(mdl, v->model, sizeof(mdl));
    ksnprintf(hw, sizeof(hw), "WINDISK\\%s&%s&%s,WINDISK\\%s&%s", b, what, mdl, b, what);
    if (v->model[0]) strlcpy(desc, v->model, sizeof(desc));
    else ksnprintf(desc, sizeof(desc), "WINDISK %s %s", b, v->cdrom ? "CD-ROM" : "Disk");
    p = pnp_child(parent, id, hw, v->cdrom ? "WINDISK\\CDROM" : "WINDISK\\DISK", desc, PK_IOSDISK, v->key);
    if (!p) return;
    p->bdf = v->key2;
    /* installed: the INF wrote its generic name, the model string goes back */
    if (!p->state && (p->laststatus & DN_STARTED)) {
        pnp_set_desc(p->dn, desc);
        p->state = 1;
    }
}

#define MAXVIEW 32

void pkg_sync(void)
{
    struct diskview *v;
    int i, n = 0;
    v = vmm_heap_alloc(MAXVIEW * sizeof(*v));
    if (!v) {
        for (i = 0; i < pnp_nnodes; i++) pnp_nodes[i].seen = 1;
        return;
    }
    vmm_begin_crit();
    if (blk_poll_hook) blk_poll_hook();         /* SD cards inserted / removed */
    for (i = 0; i < blk_count() && n < MAXVIEW; i++) {
        struct blkdev *d = blk_get(i);
        struct diskview *x = &v[n];
        int chan = 0, native = 0;
        if (!d->present && !d->cdrom) continue;
        memset(x, 0, sizeof(*x));
        x->key = d->key;
        x->key2 = d->key2;
        x->kind = d->kind;
        x->cdrom = d->cdrom;
        strlcpy(x->model, d->model, sizeof(x->model));
        switch (d->kind) {
        case DK_IDE:
            if (ide_dev_info(d->key, &x->parent_bdf, &chan, &native)) continue;
            x->chan = (u8)chan;
            break;
        case DK_AHCI:
        case DK_NVME:
            x->parent_bdf = d->key >> 16;
            break;
        case DK_SD:
        case DK_MMC:
            x->parent_bdf = d->key & 0xFFFF;
            break;
        default:
            continue;
        }
        n++;
    }
    vmm_end_crit();
    for (i = 0; i < n; i++) create_child(&v[i]);
    vmm_heap_free(v);
}

/* disks of a devnode for the port driver (pnpblk.c): the child devnode of the disk */
int pkg_blk_owned(struct pnode *p, struct blkdev *d)
{
    return p->kind == PK_IOSDISK && p->key == d->key && p->bdf == d->key2;
}
