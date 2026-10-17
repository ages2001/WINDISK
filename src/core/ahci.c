/* ahci.c - AHCI SATA disk driver (polled, single command slot)
   Ports that the BIOS already runs are used in "borrow" mode: the BIOS command
   list / FIS pointers are saved before every command and restored afterwards,
   so INT 13h keeps working on the same controller. */
#include "core.h"

#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28

#define PX_CLB      0x00
#define PX_CLBU     0x04
#define PX_FB       0x08
#define PX_FBU      0x0C
#define PX_IS       0x10
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20
#define PX_SIG      0x24
#define PX_SSTS     0x28
#define PX_SCTL     0x2C
#define PX_SERR     0x30
#define PX_SACT     0x34
#define PX_CI       0x38

#define CMD_ST      0x0001
#define CMD_SUD     0x0002
#define CMD_POD     0x0004
#define CMD_CLO     0x0008
#define CMD_FRE     0x0010
#define CMD_FR      0x4000
#define CMD_CR      0x8000

#define IS_TFES     0x40000000UL
#define IS_FATAL    0x78000000UL

#define MAXCTRL 4
#define MAXPORT 32

struct ahci_port {
    struct blkdev bd;
    struct ahci_ctrl *c;
    u8 *regs;
    int no;
    int borrow;         /* BIOS owns the port: save/restore around commands */
    int lba48;
    int atapi;          /* CD/DVD drive (cdrom.c) */
    u8 *clb;            /* 1K command list */
    u8 *fis;            /* 256 byte receive FIS */
    u8 *ct;             /* command table */
    u32 b_clb, b_clbu, b_fb, b_fbu, b_cmd;
};

struct ahci_ctrl {
    struct pcidev pci;
    u8 *abar;
    u32 cap;
    int nports;
    struct ahci_port *ports[MAXPORT];
};

static struct ahci_ctrl *ctrls[MAXCTRL];
static int nctrl;

static int wait_clear(u8 *reg, u32 mask, u32 ms)
{
    u32 t0 = plat_ms();
    while (RD32(reg) & mask) {
        if (plat_ms() - t0 > ms) return -1;
        cpu_pause();
    }
    return 0;
}

static int port_stop(u8 *p)
{
    u32 cmd = RD32(p + PX_CMD);
    if (cmd & CMD_ST) WR32(p + PX_CMD, cmd & ~CMD_ST);
    if (wait_clear(p + PX_CMD, CMD_CR, 500)) return -1;
    cmd = RD32(p + PX_CMD);
    if (cmd & CMD_FRE) WR32(p + PX_CMD, cmd & ~CMD_FRE);
    if (wait_clear(p + PX_CMD, CMD_FR, 500)) return -1;
    return 0;
}

static void port_start(u8 *p)
{
    u32 t0;
    wait_clear(p + PX_CMD, CMD_CR, 500);
    WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_FRE);
    t0 = plat_ms();
    while (!(RD32(p + PX_CMD) & CMD_FR) && plat_ms() - t0 < 100) cpu_pause();
    WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_ST);
}

static void port_program(struct ahci_port *ap)
{
    u8 *p = ap->regs;
    WR32(p + PX_CLB, vphys(ap->clb));
    WR32(p + PX_CLBU, 0);
    WR32(p + PX_FB, vphys(ap->fis));
    WR32(p + PX_FBU, 0);
    WR32(p + PX_SERR, 0xFFFFFFFFUL);
    WR32(p + PX_IS, 0xFFFFFFFFUL);
}

/* A port the BIOS has set up: running, or with a command list / FIS area programmed.
   UEFI-CSM AHCI code (EDK2) stops the port after every command, so ST/FRE alone do not
   tell; its PxCLB/PxFB stay programmed and must be restored after each of our commands. */
static int bios_port(u8 *p)
{
    if (RD32(p + PX_CMD) & (CMD_ST | CMD_FRE | CMD_CR | CMD_FR)) return 1;
    if (RD32(p + PX_CLB) || RD32(p + PX_CLBU) || RD32(p + PX_FB) || RD32(p + PX_FBU)) return 1;
    return 0;
}

static void borrow_begin(struct ahci_port *ap)
{
    u8 *p = ap->regs;
    if (!ap->borrow) return;
    ap->b_cmd = RD32(p + PX_CMD);
    ap->b_clb = RD32(p + PX_CLB);
    ap->b_clbu = RD32(p + PX_CLBU);
    ap->b_fb = RD32(p + PX_FB);
    ap->b_fbu = RD32(p + PX_FBU);
    port_stop(p);
    port_program(ap);
    port_start(p);
}

static void borrow_end(struct ahci_port *ap)
{
    u8 *p = ap->regs;
    if (!ap->borrow) return;
    port_stop(p);
    WR32(p + PX_CLB, ap->b_clb);
    WR32(p + PX_CLBU, ap->b_clbu);
    WR32(p + PX_FB, ap->b_fb);
    WR32(p + PX_FBU, ap->b_fbu);
    WR32(p + PX_SERR, 0xFFFFFFFFUL);
    WR32(p + PX_IS, 0xFFFFFFFFUL);
    if (ap->b_cmd & CMD_FRE) WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_FRE);
    if (ap->b_cmd & CMD_ST) WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_ST);
}

static void port_recover(struct ahci_port *ap)
{
    u8 *p = ap->regs;
    u32 tfd;
    port_stop(p);
    WR32(p + PX_SERR, 0xFFFFFFFFUL);
    WR32(p + PX_IS, 0xFFFFFFFFUL);
    tfd = RD32(p + PX_TFD);
    if (tfd & 0x88) {
        /* BSY/DRQ stuck: command list override, then COMRESET if needed */
        if (ap->c->cap & (1UL << 24)) {
            WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_CLO);
            wait_clear(p + PX_CMD, CMD_CLO, 500);
        }
        if (RD32(p + PX_TFD) & 0x88) {
            WR32(p + PX_SCTL, (RD32(p + PX_SCTL) & ~0xFUL) | 1);
            mdelay(2);
            WR32(p + PX_SCTL, RD32(p + PX_SCTL) & ~0xFUL);
            mdelay(50);
            WR32(p + PX_SERR, 0xFFFFFFFFUL);
            wait_clear(p + PX_TFD, 0x88, 3000);
        }
    }
    port_start(p);
}

/* issue a single ATA command in slot 0; buf must be pool memory */
static int ahci_exec(struct ahci_port *ap, u8 cmd, u64 lba, u32 count, void *buf, u32 bytes, int write)
{
    u8 *p = ap->regs;
    u8 *f = ap->ct;
    u32 *hdr = (u32 *)ap->clb;
    u32 *prd = (u32 *)(ap->ct + 0x80);
    u32 t0, is, ret = 0;

    borrow_begin(ap);
    if (RD32(p + PX_TFD) & 0x88) port_recover(ap);
    memset(f, 0, 0x90);
    f[0] = 0x27;                /* H2D register FIS */
    f[1] = 0x80;                /* command */
    f[2] = cmd;
    f[4] = (u8)lba;
    f[5] = (u8)(lba >> 8);
    f[6] = (u8)(lba >> 16);
    f[7] = 0x40;                /* LBA mode */
    f[8] = (u8)(lba >> 24);
    f[9] = (u8)(lba >> 32);
    f[10] = (u8)(lba >> 40);
    f[12] = (u8)count;
    f[13] = (u8)(count >> 8);
    if (!ap->lba48 && cmd != 0xEC) f[7] |= (u8)((lba >> 24) & 0x0F);
    hdr[0] = 5 | (write ? 0x40 : 0) | ((bytes ? 1UL : 0UL) << 16);
    hdr[1] = 0;
    hdr[2] = vphys(ap->ct);
    hdr[3] = 0;
    if (bytes) {
        prd[0] = vphys(buf);
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = (bytes - 1) & 0x3FFFFFUL;
    }
    WR32(p + PX_IS, 0xFFFFFFFFUL);
    WR32(p + PX_CI, 1);
    t0 = plat_ms();
    for (;;) {
        is = RD32(p + PX_IS);
        if (is & (IS_TFES | IS_FATAL)) { ret = 1; break; }
        if (!(RD32(p + PX_CI) & 1)) break;
        if (plat_ms() - t0 > 10000) { ret = 2; break; }
        cpu_pause();
    }
    if (ret) {
        kprintf("ahci%d.%d: cmd %02x lba %lu failed (%s) IS=%08lx TFD=%08lx SERR=%08lx\n",
                ap->c->pci.dev, ap->no, cmd, (u32)lba, ret == 2 ? "timeout" : "error",
                is, RD32(p + PX_TFD), RD32(p + PX_SERR));
        port_recover(ap);
        borrow_end(ap);
        return -1;
    }
    if (RD32(p + PX_TFD) & 0x01) {
        borrow_end(ap);
        return -1;
    }
    borrow_end(ap);
    return 0;
}

static int ahci_rw(struct blkdev *d, u64 lba, u32 cnt, void *buf, int wr)
{
    struct ahci_port *ap = d->priv;
    u8 cmd;
    if (ap->lba48) cmd = wr ? 0x35 : 0x25;
    else cmd = wr ? 0xCA : 0xC8;
    return ahci_exec(ap, cmd, lba, cnt, buf, cnt * d->ssize, wr);
}

static int ahci_flush(struct blkdev *d)
{
    struct ahci_port *ap = d->priv;
    return ahci_exec(ap, ap->lba48 ? 0xEA : 0xE7, 0, 0, NULL, 0, 0);
}

static int ahci_ready(struct blkdev *d)
{
    (void)d;
    return 0;
}

static struct blkops ahci_ops = { ahci_rw, ahci_flush, ahci_ready };

/* ATAPI PACKET command with DMA data in; 0 ok, 1 check condition, -1 error */
static int ahci_packet(void *priv, const u8 *cdb, void *buf, u32 len, u32 ms)
{
    struct ahci_port *ap = priv;
    u8 *p = ap->regs;
    u8 *f = ap->ct;
    u32 *hdr = (u32 *)ap->clb;
    u32 *prd = (u32 *)(ap->ct + 0x80);
    u32 t0, is;
    int ret = 0;

    borrow_begin(ap);
    if (RD32(p + PX_TFD) & 0x88) port_recover(ap);
    memset(f, 0, 0x90);
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = 0xA0;                /* PACKET */
    f[3] = (u8)(len ? 1 : 0);   /* DMA */
    f[5] = 0xFE;                /* byte count limit (PIO only) */
    f[6] = 0xFF;
    memcpy(f + 0x40, cdb, 12);
    hdr[0] = 5 | 0x20 | ((len ? 1UL : 0UL) << 16);     /* ATAPI */
    hdr[1] = 0;
    hdr[2] = vphys(ap->ct);
    hdr[3] = 0;
    if (len) {
        prd[0] = vphys(buf);
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = (len - 1) & 0x3FFFFFUL;
    }
    WR32(p + PX_IS, 0xFFFFFFFFUL);
    WR32(p + PX_CI, 1);
    t0 = plat_ms();
    for (;;) {
        is = RD32(p + PX_IS);
        if (is & IS_TFES) { ret = 1; break; }   /* CHECK CONDITION: sense data wanted */
        if (is & IS_FATAL) { ret = -1; break; }
        if (!(RD32(p + PX_CI) & 1)) break;
        if (plat_ms() - t0 > ms) { ret = -1; break; }
        cpu_pause();
    }
    if (!ret && (RD32(p + PX_TFD) & 0x01)) ret = 1;
    if (ret) port_recover(ap);
    borrow_end(ap);
    return ret;
}

static void port_init(struct ahci_ctrl *c, int no)
{
    u8 *p = c->abar + 0x100 + no * 0x80;
    u32 ssts = RD32(p + PX_SSTS), sig, cmd;
    struct ahci_port *ap;
    u16 *id;
    u32 w106;

    if ((ssts & 0xF) != 3) return;
    sig = RD32(p + PX_SIG);
    if (sig == 0x96690101UL || sig == 0xC33C0101UL) {
        kprintf("ahci: port %d port multiplier/SEMB, not handled\n", no);
        return;
    }
    ap = kmalloc(sizeof(*ap));
    ap->c = c;
    ap->regs = p;
    ap->no = no;
    ap->clb = kmalloc_align(1024, 1024);
    ap->fis = kmalloc_align(256, 256);
    ap->ct = kmalloc_align(0x100, 128);
    cmd = RD32(p + PX_CMD);
    ap->borrow = bios_port(p);
    if (!ap->borrow) {
        port_stop(p);
        WR32(p + PX_IE, 0);
        if (c->cap & (1UL << 27)) WR32(p + PX_CMD, RD32(p + PX_CMD) | CMD_SUD | CMD_POD);
        port_program(ap);
        port_start(p);
    }
    id = kmalloc_align(512, 512);
    if (sig == 0xEB140101UL) {
        /* ATAPI: CD/DVD drives */
        char model[41], serial[21];
        ap->atapi = 1;
        if (ahci_exec(ap, 0xA1, 0, 0, id, 512, 0) || ((id[0] >> 8) & 0x1F) != 5) {
            kprintf("ahci: port %d ATAPI device, not a CD/DVD drive\n", no);
            kfree(id);
            return;
        }
        trim_ata_string(model, id + 27, 20);
        trim_ata_string(serial, id + 10, 10);
        kfree(id);
        kprintf("ahci%02x:%02x port %d: %s (CD/DVD)%s\n", c->pci.bus, c->pci.dev, no, model,
                ap->borrow ? ", shared with BIOS" : "");
        c->ports[no] = ap;
        cd_register(model, serial, DK_AHCI, (PCI_BDF(&c->pci) << 16) | (u32)no, ahci_packet, ap);
        return;
    }
    if (ahci_exec(ap, 0xEC, 0, 0, id, 512, 0)) {
        kprintf("ahci: port %d IDENTIFY failed\n", no);
        kfree(id);
        return;
    }
    ap->lba48 = (id[83] & (1 << 10)) != 0;
    ap->bd.ops = &ahci_ops;
    ap->bd.priv = ap;
    ap->bd.kind = DK_AHCI;
    ap->bd.present = 1;
    ap->bd.key = (PCI_BDF(&c->pci) << 16) | (u32)no;
    ap->bd.ssize = 512;
    w106 = id[106];
    if ((w106 & 0xC000) == 0x4000 && (w106 & 0x1000))
        ap->bd.ssize = ((u32)id[117] | ((u32)id[118] << 16)) * 2;
    if (ap->bd.ssize < 512 || ap->bd.ssize > 4096) ap->bd.ssize = 512;
    if (ap->lba48)
        ap->bd.sectors = (u64)id[100] | ((u64)id[101] << 16) | ((u64)id[102] << 32) | ((u64)id[103] << 48);
    else
        ap->bd.sectors = (u32)id[60] | ((u32)id[61] << 16);
    ap->bd.maxxfer = ap->lba48 ? BOUNCE_SIZE / ap->bd.ssize : MIN(256, BOUNCE_SIZE / ap->bd.ssize);
    trim_ata_string(ap->bd.model, id + 27, 20);
    trim_ata_string(ap->bd.serial, id + 10, 10);
    ap->bd.key2 = crc32(0, ap->bd.serial, strlen(ap->bd.serial));
    kfree(id);
    kprintf("ahci%02x:%02x port %d: %s, %lu MB%s%s\n", c->pci.bus, c->pci.dev, no, ap->bd.model,
            (u32)(ap->bd.sectors / (1048576UL / ap->bd.ssize)), ap->lba48 ? ", LBA48" : "",
            ap->borrow ? ", shared with BIOS" : "");
    c->ports[no] = ap;
    blk_register(&ap->bd);
}

int ahci_probe(struct pcidev *pd)
{
    struct ahci_ctrl *c;
    u32 phys = pci_bar_mem(pd, 5, NULL), pi, ghc, vs;
    int i;
    if (!phys || nctrl >= MAXCTRL) return -1;
    pci_enable(pd, 1, 0, 1);
    c = kmalloc(sizeof(*c));
    c->pci = *pd;
    c->abar = plat_mmio(phys, 0x1100);
    if (!c->abar) return -1;
    vs = RD32(c->abar + HBA_VS);
    if (vs == 0xFFFFFFFFUL || vs < 0x00010000UL) {
        kprintf("ahci: %02x:%02x.%u %04x:%04x has no AHCI registers (version %08lx)\n", pd->bus, pd->dev, pd->fn,
                pd->vendor, pd->device, vs);
        return -1;
    }
    c->cap = RD32(c->abar + HBA_CAP);
    /* The BIOS keeps using the controller (INT 13h), so it is not asked to give it up
       (BIOS/OS handoff) and the global interrupt enable stays as the BIOS set it: BIOS
       disk code may wait for completion interrupts. Our ports are polled. */
    ghc = RD32(c->abar + HBA_GHC);
    if (!(ghc & 0x80000000UL)) WR32(c->abar + HBA_GHC, ghc | 0x80000000UL);
    pi = RD32(c->abar + HBA_PI);
    c->nports = (c->cap & 0x1F) + 1;
    kprintf("ahci: %02x:%02x.%u %04x:%04x ver %08lx, %d ports, PI %08lx\n", pd->bus, pd->dev, pd->fn,
            pd->vendor, pd->device, RD32(c->abar + HBA_VS), c->nports, pi);
    ctrls[nctrl++] = c;
    for (i = 0; i < MAXPORT; i++)
        if (pi & (1UL << i)) port_init(c, i);
    return 0;
}

void ahci_shutdown(void)
{
    int i, j;
    for (i = 0; i < nctrl; i++) {
        struct ahci_ctrl *c = ctrls[i];
        for (j = 0; j < MAXPORT; j++) {
            struct ahci_port *ap = c->ports[j];
            if (!ap) continue;
            if (!ap->atapi) ahci_flush(&ap->bd);
            if (!ap->borrow) {
                /* leave the port as found: stopped and unprogrammed */
                port_stop(ap->regs);
                WR32(ap->regs + PX_CLB, 0);
                WR32(ap->regs + PX_CLBU, 0);
                WR32(ap->regs + PX_FB, 0);
                WR32(ap->regs + PX_FBU, 0);
            }
        }
    }
}

/* re-take ports after another owner (Windows) used the controller */
void ahci_resume(void)
{
    int i, j;
    for (i = 0; i < nctrl; i++) {
        struct ahci_ctrl *c = ctrls[i];
        WR32(c->abar + HBA_GHC, RD32(c->abar + HBA_GHC) | 0x80000000UL);
        for (j = 0; j < MAXPORT; j++) {
            struct ahci_port *ap = c->ports[j];
            if (!ap) continue;
            ap->borrow = bios_port(ap->regs);
            if (!ap->borrow) {
                port_stop(ap->regs);
                port_program(ap);
                port_start(ap->regs);
            }
        }
    }
}

/* the controller at bdf goes away (Plug and Play stop): its disks are flushed and dropped,
   the ports we programmed are left stopped */
void ahci_remove(u32 bdf)
{
    int i, j;
    for (i = 0; i < nctrl; i++) {
        struct ahci_ctrl *c = ctrls[i];
        int cds = 0;
        if (PCI_BDF(&c->pci) != bdf) continue;
        for (j = 0; j < MAXPORT; j++) {
            struct ahci_port *ap = c->ports[j];
            if (!ap) continue;
            if (ap->atapi) {
                cds++;              /* registered with cdrom.c, which keeps using the port */
                continue;
            }
            ahci_flush(&ap->bd);
            blk_unregister(&ap->bd);
            if (!ap->borrow) {
                port_stop(ap->regs);
                WR32(ap->regs + PX_CLB, 0);
                WR32(ap->regs + PX_CLBU, 0);
                WR32(ap->regs + PX_FB, 0);
                WR32(ap->regs + PX_FBU, 0);
            }
            c->ports[j] = NULL;
        }
        if (cds) continue;
        ctrls[i] = ctrls[--nctrl];
        return;
    }
}
