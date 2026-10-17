/* ide.c - ATA disks on IDE controllers: legacy (compatibility) and native PCI mode,
   PIO and bus-master DMA, CHS / LBA28 / LBA48 */
#include "core.h"

#define MAXCHAN     8

/* task file */
#define R_DATA      0
#define R_ERR       1
#define R_FEAT      1
#define R_COUNT     2
#define R_LBA0      3
#define R_LBA1      4
#define R_LBA2      5
#define R_DEV       6
#define R_STATUS    7
#define R_CMD       7

#define ST_ERR      0x01
#define ST_DRQ      0x08
#define ST_DF       0x20
#define ST_DRDY     0x40
#define ST_BSY      0x80

/* bus master */
#define BM_CMD      0
#define BM_STATUS   2
#define BM_PRD      4

struct ide_chan;

struct ide_dev {
    struct blkdev bd;
    struct ide_chan *ch;
    u8 slave;
    u8 lba;             /* LBA addressing supported */
    u8 lba48;
    u8 dma;             /* bus-master DMA usable */
    u8 atapi;           /* CD/DVD drive (cdrom.c) */
    u16 cyls, heads, spt;
};

struct ide_chan {
    u16 cmd, ctl, bm;   /* bm = 0: no bus master */
    u8 irq;
    u8 native;
    u8 haspci;
    u8 owned;           /* not shared with the BIOS: polled with nIEN set */
    u8 cdonly;          /* VxD: a channel the BIOS keeps, only its CD drives are used */
    u8 no;              /* 0 primary, 1 secondary */
    struct pcidev pci;
    struct ide_dev *dev[2];
    u32 *prd;
};

static struct ide_chan *chans[MAXCHAN];
static int nchan;

/* rep insw / rep outsw with a flat ES */
void ide_insw(u16 port, void *buf, u32 n);
#pragma aux ide_insw = "rep insw" parm [edx] [edi] [ecx] modify exact [edi ecx];
void ide_outsw(u16 port, const void *buf, u32 n);
#pragma aux ide_outsw = "rep outsw" parm [edx] [esi] [ecx] modify exact [esi ecx];

static u8 altst(struct ide_chan *c)
{
    return inb(c->ctl);
}

static void delay400(struct ide_chan *c)
{
    altst(c); altst(c); altst(c); altst(c);
}

/* wait for BSY to clear; returns the status or -1 on timeout / floating bus */
static int wait_bsy(struct ide_chan *c, u32 ms)
{
    u32 t0 = plat_ms();
    for (;;) {
        u8 s = altst(c);
        if (s == 0xFF) return -1;
        if (!(s & ST_BSY)) return s;
        if (plat_ms() - t0 > ms) return -1;
        cpu_pause();
    }
}

/* wait until BSY is clear and DRQ or ERR is set */
static int wait_drq(struct ide_chan *c, u32 ms)
{
    u32 t0 = plat_ms();
    for (;;) {
        u8 s = altst(c);
        if (s == 0xFF) return -1;
        if (!(s & ST_BSY) && (s & (ST_DRQ | ST_ERR | ST_DF))) return s;
        if (plat_ms() - t0 > ms) return -1;
        cpu_pause();
    }
}

static void select_dev(struct ide_chan *c, int slave, u8 bits)
{
    outb(c->cmd + R_DEV, (u8)(0xA0 | (slave ? 0x10 : 0) | bits));
    delay400(c);
}

/* program the task file. CHS when the device has no LBA support. */
static void set_taskfile(struct ide_dev *d, u64 lba, u32 count)
{
    struct ide_chan *c = d->ch;
    if (d->lba48) {
        select_dev(c, d->slave, 0x40);
        outb(c->cmd + R_COUNT, (u8)(count >> 8));
        outb(c->cmd + R_LBA0, (u8)(lba >> 24));
        outb(c->cmd + R_LBA1, (u8)(lba >> 32));
        outb(c->cmd + R_LBA2, (u8)(lba >> 40));
        outb(c->cmd + R_COUNT, (u8)count);
        outb(c->cmd + R_LBA0, (u8)lba);
        outb(c->cmd + R_LBA1, (u8)(lba >> 8));
        outb(c->cmd + R_LBA2, (u8)(lba >> 16));
    } else if (d->lba) {
        select_dev(c, d->slave, (u8)(0x40 | ((u32)lba >> 24 & 0x0F)));
        outb(c->cmd + R_COUNT, (u8)count);
        outb(c->cmd + R_LBA0, (u8)lba);
        outb(c->cmd + R_LBA1, (u8)(lba >> 8));
        outb(c->cmd + R_LBA2, (u8)(lba >> 16));
    } else {
        u32 l = (u32)lba, cyl, head, sec;
        sec = l % d->spt + 1;
        l /= d->spt;
        head = l % d->heads;
        cyl = l / d->heads;
        select_dev(c, d->slave, (u8)(head & 0x0F));
        outb(c->cmd + R_COUNT, (u8)count);
        outb(c->cmd + R_LBA0, (u8)sec);
        outb(c->cmd + R_LBA1, (u8)cyl);
        outb(c->cmd + R_LBA2, (u8)(cyl >> 8));
    }
}

static void chan_irq_off(struct ide_chan *c)
{
    if (c->owned) outb(c->ctl, 0x02);   /* nIEN: polled operation */
}

static int pio_rw(struct ide_dev *d, u64 lba, u32 cnt, u8 *buf, int wr)
{
    struct ide_chan *c = d->ch;
    u32 i;
    int s;
    u8 cmd;
    if (d->lba48) cmd = wr ? 0x34 : 0x24;
    else cmd = wr ? 0x30 : 0x20;
    if (wait_bsy(c, 5000) < 0) return -1;
    set_taskfile(d, lba, cnt);
    outb(c->cmd + R_CMD, cmd);
    for (i = 0; i < cnt; i++) {
        s = wait_drq(c, 10000);
        if (s < 0 || (s & (ST_ERR | ST_DF)) || !(s & ST_DRQ)) return -1;
        if (wr) ide_outsw(c->cmd + R_DATA, buf + i * d->bd.ssize, d->bd.ssize / 2);
        else ide_insw(c->cmd + R_DATA, buf + i * d->bd.ssize, d->bd.ssize / 2);
    }
    s = wait_bsy(c, 10000);
    if (s < 0 || (s & (ST_ERR | ST_DF))) return -1;
    (void)inb(c->cmd + R_STATUS);
    return 0;
}

/* physical region descriptors: 64K maximum per entry, no 64K boundary crossing */
static int build_prd(struct ide_chan *c, const void *buf, u32 bytes)
{
    u32 p = vphys(buf);
    int n = 0;
    while (bytes) {
        u32 room = 0x10000UL - (p & 0xFFFF), len = bytes < room ? bytes : room;
        if (n >= 31) return -1;
        c->prd[n * 2] = p;
        c->prd[n * 2 + 1] = len & 0xFFFF;   /* 0 = 64K */
        p += len;
        bytes -= len;
        n++;
    }
    c->prd[n * 2 - 1] |= 0x80000000UL;
    return 0;
}

static int dma_rw(struct ide_dev *d, u64 lba, u32 cnt, u8 *buf, int wr)
{
    struct ide_chan *c = d->ch;
    u32 t0;
    u8 st, bs, cmd;
    int s;
    if (d->lba48) cmd = wr ? 0x35 : 0x25;
    else cmd = wr ? 0xCA : 0xC8;
    if (build_prd(c, buf, cnt * d->bd.ssize)) return -1;
    if (wait_bsy(c, 5000) < 0) return -1;
    outb(c->bm + BM_CMD, 0);
    outl(c->bm + BM_PRD, vphys(c->prd));
    outb(c->bm + BM_STATUS, (u8)(inb(c->bm + BM_STATUS) | 0x06));   /* clear interrupt, error */
    outb(c->bm + BM_CMD, wr ? 0x00 : 0x08);
    set_taskfile(d, lba, cnt);
    outb(c->cmd + R_CMD, cmd);
    outb(c->bm + BM_CMD, (u8)((wr ? 0x00 : 0x08) | 0x01));
    t0 = plat_ms();
    for (;;) {
        bs = inb(c->bm + BM_STATUS);
        st = altst(c);
        if (bs & 0x02) break;                       /* bus master error */
        if (!(bs & 0x01) && !(st & ST_BSY)) break;  /* transfer finished */
        if ((st & (ST_ERR | ST_DF)) && !(st & ST_BSY)) break;
        if (plat_ms() - t0 > 10000) break;
        cpu_pause();
    }
    outb(c->bm + BM_CMD, 0);
    s = wait_bsy(c, 1000);
    outb(c->bm + BM_STATUS, (u8)(inb(c->bm + BM_STATUS) | 0x06));
    (void)inb(c->cmd + R_STATUS);
    if ((bs & 0x03) != 0 || s < 0 || (s & (ST_ERR | ST_DF))) return -1;
    return 0;
}

static void soft_reset(struct ide_chan *c)
{
    outb(c->ctl, (u8)(0x04 | (c->owned ? 0x02 : 0)));
    udelay(10);
    outb(c->ctl, (u8)(c->owned ? 0x02 : 0));
    mdelay(5);
    wait_bsy(c, 3000);
}

static int ide_rw(struct blkdev *bd, u64 lba, u32 cnt, void *buf, int wr)
{
    struct ide_dev *d = bd->priv;
    int r;
    chan_irq_off(d->ch);
    if (d->dma) {
        r = dma_rw(d, lba, cnt, buf, wr);
        if (!r) return 0;
        kprintf("ide: DMA error on %s, using PIO\n", bd->model);
        soft_reset(d->ch);
        d->dma = 0;
    }
    r = pio_rw(d, lba, cnt, buf, wr);
    if (r) {
        soft_reset(d->ch);
        r = pio_rw(d, lba, cnt, buf, wr);
    }
    return r ? -1 : 0;
}

static int ide_flush(struct blkdev *bd)
{
    struct ide_dev *d = bd->priv;
    struct ide_chan *c = d->ch;
    int s;
    chan_irq_off(c);
    if (wait_bsy(c, 5000) < 0) return -1;
    select_dev(c, d->slave, 0);
    outb(c->cmd + R_CMD, d->lba48 ? 0xEA : 0xE7);
    s = wait_bsy(c, 30000);
    (void)inb(c->cmd + R_STATUS);
    return s < 0 ? -1 : 0;
}

static int ide_ready(struct blkdev *bd)
{
    (void)bd;
    return 0;
}

static struct blkops ide_ops = { ide_rw, ide_flush, ide_ready };

/* ATAPI PACKET command, PIO data in; 0 ok, 1 check condition, -1 error */
static int atapi_packet(void *priv, const u8 *cdb, void *buf, u32 len, u32 ms)
{
    struct ide_dev *d = priv;
    struct ide_chan *c = d->ch;
    u8 *p = buf;
    u32 got = 0;
    int s;
    chan_irq_off(c);
    if (wait_bsy(c, 5000) < 0) {
        if (!c->owned) return -1;           /* the BIOS disk on the channel would be reset too */
        soft_reset(c);
        if (wait_bsy(c, 5000) < 0) return -1;
    }
    select_dev(c, d->slave, 0);
    if (wait_bsy(c, 5000) < 0) return -1;
    outb(c->cmd + R_FEAT, 0);               /* PIO */
    outb(c->cmd + R_LBA1, 0xFE);            /* byte count limit */
    outb(c->cmd + R_LBA2, 0xFF);
    outb(c->cmd + R_CMD, 0xA0);
    delay400(c);
    s = wait_drq(c, 3000);
    if (s < 0) return -1;
    if (s & ST_ERR) {
        (void)inb(c->cmd + R_STATUS);
        return 1;
    }
    ide_outsw(c->cmd + R_DATA, cdb, 6);
    for (;;) {
        u32 bc, words, take;
        delay400(c);
        s = wait_bsy(c, ms);
        if (s < 0) {
            if (c->owned) soft_reset(c);
            return -1;
        }
        if (s & ST_ERR) {
            (void)inb(c->cmd + R_STATUS);
            return 1;
        }
        if (!(s & ST_DRQ)) break;
        bc = inb(c->cmd + R_LBA1) | ((u32)inb(c->cmd + R_LBA2) << 8);
        words = (bc + 1) / 2;
        take = got < len ? MIN(words, (len - got) / 2) : 0;
        if (take) ide_insw(c->cmd + R_DATA, p + got, take);
        got += take * 2;
        for (; take < words; take++) (void)inw(c->cmd + R_DATA);
    }
    (void)inb(c->cmd + R_STATUS);
    return 0;
}

/* IDENTIFY PACKET DEVICE */
static int identify_packet(struct ide_chan *c, int slave, u16 *id)
{
    int st;
    select_dev(c, slave, 0);
    if (wait_bsy(c, 3000) < 0) return -1;
    outb(c->cmd + R_CMD, 0xA1);
    delay400(c);
    st = wait_drq(c, 3000);
    if (st < 0 || (st & ST_ERR) || !(st & ST_DRQ)) return -1;
    ide_insw(c->cmd + R_DATA, id, 256);
    (void)inb(c->cmd + R_STATUS);
    return 0;
}

/* IDENTIFY DEVICE; returns 0 = ATA disk, 1 = ATAPI / not a disk, -1 = nothing */
static int identify(struct ide_chan *c, int slave, u16 *id)
{
    u8 s, m, h;
    int st;
    select_dev(c, slave, 0);
    s = altst(c);
    if (s == 0xFF || s == 0x7F) return -1;
    /* register test: a missing device does not keep the values */
    outb(c->cmd + R_COUNT, 0x55);
    outb(c->cmd + R_LBA0, 0xAA);
    outb(c->cmd + R_COUNT, 0xAA);
    outb(c->cmd + R_LBA0, 0x55);
    if (inb(c->cmd + R_COUNT) != 0xAA || inb(c->cmd + R_LBA0) != 0x55) return -1;
    if (wait_bsy(c, 3000) < 0) return -1;
    m = inb(c->cmd + R_LBA1);
    h = inb(c->cmd + R_LBA2);
    if ((m == 0x14 && h == 0xEB) || (m == 0x69 && h == 0x96)) return 1;
    outb(c->cmd + R_CMD, 0xEC);
    delay400(c);
    if (altst(c) == 0) return -1;
    st = wait_drq(c, 3000);
    if (st < 0) return -1;
    m = inb(c->cmd + R_LBA1);
    h = inb(c->cmd + R_LBA2);
    if ((m == 0x14 && h == 0xEB) || (m == 0x69 && h == 0x96)) return 1;
    if ((st & ST_ERR) || !(st & ST_DRQ)) return -1;
    ide_insw(c->cmd + R_DATA, id, 256);
    (void)inb(c->cmd + R_STATUS);
    return 0;
}

static void probe_dev(struct ide_chan *c, int slave)
{
    u16 *id = kmalloc_align(512, 16);
    struct ide_dev *d;
    int r = identify(c, slave, id);
    if (r > 0) {
        /* ATAPI: CD/DVD drives (peripheral type 5 in word 0) */
        if (identify_packet(c, slave, id) == 0 && ((id[0] >> 8) & 0x1F) == 5) {
            char model[41], serial[21];
            d = kmalloc(sizeof(*d));
            d->ch = c;
            d->slave = (u8)slave;
            d->atapi = 1;
            trim_ata_string(model, id + 27, 20);
            trim_ata_string(serial, id + 10, 10);
            kprintf("ide: %s %04x %s: %s (CD/DVD)\n", c->native ? "native" : "legacy", c->cmd,
                    slave ? "slave" : "master", model);
            c->dev[slave] = d;
            cd_register(model, serial, DK_IDE, ((u32)c->cmd << 16) | (u32)slave, atapi_packet, d);
        } else {
            kprintf("ide: %04x %s: ATAPI device, not a CD/DVD drive\n", c->cmd, slave ? "slave" : "master");
        }
        kfree(id);
        return;
    }
    if (r || c->cdonly) {
        kfree(id);
        return;
    }
    d = kmalloc(sizeof(*d));
    d->ch = c;
    d->slave = (u8)slave;
    d->lba = (id[49] & 0x0200) != 0;
    d->lba48 = d->lba && (id[83] & 0x0400) && (id[86] & 0x0400);
    d->cyls = id[1];
    d->heads = id[3];
    d->spt = id[6];
    if (id[53] & 1) {                       /* current translation valid */
        if (id[54] && id[55] && id[56]) {
            d->cyls = id[54];
            d->heads = id[55];
            d->spt = id[56];
        }
    }
    if (!d->heads) d->heads = 16;
    if (!d->spt) d->spt = 63;
    d->bd.ops = &ide_ops;
    d->bd.priv = d;
    d->bd.kind = DK_IDE;
    d->bd.present = 1;
    d->bd.ssize = 512;
    /* logical sectors longer than 512 bytes (word 106 bit 12, words 117-118 in words) */
    if (d->lba && (id[106] & 0xC000) == 0x4000 && (id[106] & 0x1000)) {
        u32 ls = ((u32)id[117] | ((u32)id[118] << 16)) * 2;
        if (ls == 1024 || ls == 2048 || ls == 4096) d->bd.ssize = ls;
    }
    d->bd.key = ((u32)c->cmd << 16) | (u32)slave;
    if (d->lba48)
        d->bd.sectors = (u64)id[100] | ((u64)id[101] << 16) | ((u64)id[102] << 32) | ((u64)id[103] << 48);
    else if (d->lba)
        d->bd.sectors = (u32)id[60] | ((u32)id[61] << 16);
    else
        d->bd.sectors = (u32)d->cyls * d->heads * d->spt;
    d->bd.maxxfer = 128 * 512 / d->bd.ssize;
    if (!d->lba48 && !d->lba) d->bd.maxxfer = 1;   /* CHS: never cross a track */
    /* DMA only when the controller can master and a DMA mode is active (set up by the
       BIOS / firmware; mode timing is chipset specific) */
    d->dma = 0;
    if (c->bm && d->lba && !(cb->opts & OPT_PIO) && (id[49] & 0x0100)) {
        if ((id[53] & 4) && (id[88] & 0x7F00)) d->dma = 1;     /* UDMA mode active */
        else if (id[63] & 0x0700) d->dma = 1;                  /* multiword DMA active */
        if (d->dma) outb(c->bm + BM_STATUS, (u8)(inb(c->bm + BM_STATUS) | (slave ? 0x40 : 0x20)));
    }
    trim_ata_string(d->bd.model, id + 27, 20);
    trim_ata_string(d->bd.serial, id + 10, 10);
    d->bd.key2 = crc32(0, d->bd.serial, strlen(d->bd.serial));
    kfree(id);
    kprintf("ide: %s %04x/%04x %s: %s, %lu MB, %s%s\n", c->native ? "native" : "legacy", c->cmd, c->ctl,
            slave ? "slave" : "master", d->bd.model, (u32)(d->bd.sectors >> 11),
            d->lba48 ? "LBA48" : d->lba ? "LBA28" : "CHS", d->dma ? ", DMA" : ", PIO");
    c->dev[slave] = d;
    blk_register(&d->bd);
}

static int port_claimed(u16 cmd)
{
    int i;
    for (i = 0; i < nchan; i++)
        if (chans[i]->cmd == cmd) return 1;
    return 0;
}

/* in the VxD only the channels the DOS core owned are touched; in Plug and Play mode
   (Windows 95/98/ME) the Configuration Manager gave us the whole controller */
static int chan_allowed(u16 cmd)
{
    int k;
    if (plat_mode != PM_VXD || (core_flags & CF_PNP)) return 1;
    for (k = 0; k < 8; k++)
        if (cb->ideown[k] == cmd) return 1;
    return 0;
}

static void add_chan(struct pcidev *p, int no, u16 cmd, u16 ctl, u16 bm, u8 irq, int native)
{
    struct ide_chan *c;
    if (!cmd || !ctl || nchan >= MAXCHAN || port_claimed(cmd)) return;
    c = kmalloc(sizeof(*c));
    /* in the VxD a channel the BIOS kept is used for its CD drives only, interrupts on */
    c->cdonly = (u8)!chan_allowed(cmd);
    c->cmd = cmd;
    c->ctl = ctl;
    c->bm = bm;
    c->irq = irq;
    c->no = (u8)no;
    c->native = (u8)native;
    c->owned = (u8)(plat_mode == PM_VXD && !c->cdonly);
    /* Plug and Play: a compatibility channel carries the BIOS disks, which the BIOS (real-mode
       INT 13h until the I/O Supervisor hands the drive to our port driver) may drive with
       interrupts: its interrupt enable is left alone, our commands are polled anyway */
    if ((core_flags & CF_PNP) && !native) c->owned = 0;
    if (p) {
        c->pci = *p;
        c->haspci = 1;
    }
    c->prd = kmalloc_align(256, 256);
    chans[nchan++] = c;
    probe_dev(c, 0);
    probe_dev(c, 1);
    if (!c->dev[0] && !c->dev[1]) nchan--;
    else chan_irq_off(c);
}

int ide_probe(struct pcidev *p)
{
    u8 prog = p->prog;
    u16 bm = 0, b;
    pci_enable(p, 0, 1, 1);
    if (prog & 0x80 || pci_bar_io(p, 4)) bm = (u16)pci_bar_io(p, 4);
    kprintf("ide: %02x:%02x.%u %04x:%04x prog-if %02x, primary %s, secondary %s%s\n", p->bus, p->dev, p->fn,
            p->vendor, p->device, prog, (prog & 1) ? "native" : "legacy", (prog & 4) ? "native" : "legacy",
            bm ? ", bus master" : "");
    if (prog & 1) {
        b = (u16)pci_bar_io(p, 1);
        add_chan(p, 0, (u16)pci_bar_io(p, 0), b ? (u16)(b + 2) : 0, bm, p->irq, 1);
    } else {
        add_chan(p, 0, 0x1F0, 0x3F6, bm, 14, 0);
    }
    if (prog & 4) {
        b = (u16)pci_bar_io(p, 3);
        add_chan(p, 1, (u16)pci_bar_io(p, 2), b ? (u16)(b + 2) : 0, bm ? (u16)(bm + 8) : 0, p->irq, 1);
    } else {
        add_chan(p, 1, 0x170, 0x376, bm ? (u16)(bm + 8) : 0, 15, 0);
    }
    return 0;
}

/* ISA / VLB controllers without a PCI function: the compatibility ports, and with /ISA
   the third and fourth channel */
void ide_probe_legacy(int all)
{
    if (all) {
        add_chan(NULL, 0, 0x1F0, 0x3F6, 0, 14, 0);
        add_chan(NULL, 1, 0x170, 0x376, 0, 15, 0);
    }
    if (cb->opts & OPT_ISA) {
        add_chan(NULL, 2, 0x1E8, 0x3EE, 0, 11, 0);
        add_chan(NULL, 3, 0x168, 0x36E, 0, 10, 0);
    }
}

/* does DOS serve a partition of BIOS drive 'drv' through INT 13h? */
static int dos_uses(int drv)
{
    int i;
    for (i = 0; i < cb->nmounted && i < 16; i++)
        if (cb->mounted[i].biosdrv == drv) return 1;
    return 0;
}

/* After the BIOS disks are known. A channel stays with the BIOS when DOS uses one of its
   disks through INT 13h, and the primary compatibility channel whenever it carries a BIOS
   disk (boot disk, WDCTRL in Windows). Otherwise WINDISK owns the channel and serves all
   of its disks, BIOS disks included (DOS does not use them, e.g. FAT16 LBA partitions). */
void ide_claim(void)
{
    int i, k, n = 0;
    for (i = 0; i < nchan; i++) {
        struct ide_chan *c = chans[i];
        int bios = 0, used = 0;
        for (k = 0; k < 2; k++) {
            struct ide_dev *d = c->dev[k];
            if (!d || d->atapi || !d->bd.shared) continue;
            bios = 1;
            if (dos_uses(0x80 + d->bd.shared - 1)) used = 1;
        }
        if (bios && (used || (!c->native && c->cmd == 0x1F0))) {
            for (k = 0; k < 2; k++)
                if (c->dev[k] && !c->dev[k]->atapi && !c->dev[k]->bd.shared) c->dev[k]->bd.shared = 0xFE;
            kprintf("ide: channel %04x is used by the BIOS, left alone\n", c->cmd);
            continue;
        }
        for (k = 0; k < 2; k++)
            if (c->dev[k] && !c->dev[k]->atapi) c->dev[k]->bd.shared = 0;
        c->owned = 1;
        chan_irq_off(c);
        if (n < 8) cb->ideown[n++] = c->cmd;
    }
}

void ide_shutdown(void)
{
    int i, k;
    for (i = 0; i < nchan; i++)
        for (k = 0; k < 2; k++)
            if (chans[i]->owned && chans[i]->dev[k] && !chans[i]->dev[k]->atapi) ide_flush(&chans[i]->dev[k]->bd);
}

void ide_resume(void)
{
    int i;
    for (i = 0; i < nchan; i++) chan_irq_off(chans[i]);
}

/* the PCI function, channel (0 primary, 1 secondary) and mode of the device with this key
   (IDE disks and CD/DVD drives: command port << 16 | slave); 0 found, -1 not an IDE device */
int ide_dev_info(u32 key, u32 *bdf, int *chan, int *native)
{
    int i;
    for (i = 0; i < nchan; i++) {
        struct ide_chan *c = chans[i];
        if (c->cmd != (u16)(key >> 16)) continue;
        *bdf = c->haspci ? PCI_BDF(&c->pci) : 0xFFFFFFFFUL;
        *chan = c->no;
        *native = c->native;
        return 0;
    }
    return -1;
}

/* the controller at bdf goes away (Plug and Play stop): its disks are flushed and dropped */
void ide_remove(u32 bdf)
{
    int i, k;
    for (i = 0; i < nchan; i++) {
        struct ide_chan *c = chans[i];
        if (!c->haspci || PCI_BDF(&c->pci) != bdf) continue;
        for (k = 0; k < 2; k++) {
            struct ide_dev *d = c->dev[k];
            if (!d) continue;
            if (!d->atapi) {
                ide_flush(&d->bd);
                blk_unregister(&d->bd);
                kfree(d);
                c->dev[k] = NULL;
            }
        }
        if (c->dev[0] || c->dev[1]) continue;      /* CD/DVD drives stay registered */
        kfree(c->prd);
        kfree(c);
        chans[i--] = chans[--nchan];
    }
}
