/* sdhci.c - SD / SDHC / SDXC memory cards and MMC / eMMC devices on SD Host Controllers
   (SDHCI 1.0 - 4.x): PCI class 08/05 functions and the controllers that only ACPI describes
   (Bay Trail, Cherry Trail, Braswell, Apollo Lake, AMD). Polled; ADMA2 when the controller has
   it, PIO otherwise.
   A card in a removable slot is a removable disk (slot drive letters); card changes are seen
   when DOS asks for the medium (sd_poll). Non-removable slots (embedded slot type, eMMC
   controllers, ACPI _RMV = 0 or "non-removable") and soldered eMMC devices are fixed disks.
   A card the BIOS has initialized and keeps using (INT 13h disk) is used in "borrow" mode:
   no reset, the BIOS's bus width and timing, and the controller registers the BIOS set are
   saved before and restored after each of our commands. SDIO cards (Wi-Fi) are left alone. */
#include "core.h"
#include "acpi.h"

#define R_SDMA      0x00
#define R_BLKSIZE   0x04
#define R_BLKCNT    0x06
#define R_ARG       0x08
#define R_XFER      0x0C
#define R_CMD       0x0E
#define R_RESP      0x10
#define R_DATA      0x20
#define R_STATE     0x24
#define R_HOST1     0x28
#define R_POWER     0x29
#define R_CLOCK     0x2C
#define R_TIMEOUT   0x2E
#define R_RESET     0x2F
#define R_NIS       0x30
#define R_EIS       0x32
#define R_NISEN     0x34
#define R_EISEN     0x36
#define R_NISSIG    0x38
#define R_EISSIG    0x3A
#define R_HOST2     0x3E
#define R_CAPS      0x40
#define R_CAPS2     0x44
#define R_ADMAERR   0x54
#define R_ADMA      0x58
#define R_VERSION   0xFE

#define ST_CMDINH   0x00000001UL
#define ST_DATINH   0x00000002UL
#define ST_BWEN     0x00000400UL
#define ST_BREN     0x00000800UL
#define ST_CARD     0x00010000UL
#define ST_STABLE   0x00020000UL
#define ST_WP       0x00080000UL

#define NI_CMD      0x0001
#define NI_XFER     0x0002
#define NI_BWR      0x0010
#define NI_BRD      0x0020
#define NI_ERR      0x8000

#define CAP_8BIT    0x00040000UL    /* 8-bit bus for embedded devices */
#define CAP_ADMA2   0x00080000UL
#define CAP_HS      0x00200000UL    /* high speed */

#define H1_4BIT     0x02
#define H1_HS       0x04
#define H1_DMA      0x18
#define H1_8BIT     0x20
#define H1_CDTEST   0xC0            /* card detect test level and signal selection: "inserted" */

/* response types in the command register */
#define RSP_NONE    0x00
#define RSP_136     0x09        /* R2: CRC check */
#define RSP_48      0x1A        /* R1, R6, R7: CRC + index check */
#define RSP_48B     0x1B        /* R1b */
#define RSP_R3      0x02        /* R3 (OCR): no checks */
#define DATA        0x20

/* card states (R1 bits 12:9) */
#define CS_STBY     3
#define CS_TRAN     4
#define CS_DATA     5
#define CS_RCV      6

#define MAXHOST     8
#define MAXADMA     32

/* slot kinds by controller id */
#define SDT_ANY     0
#define SDT_SD      1           /* SD card slot */
#define SDT_EMMC    2           /* eMMC: non-removable */
#define SDT_SDIO    3           /* SDIO only (Wi-Fi): not used */

struct sdsave {                 /* borrow mode: the BIOS's register values */
    u32 adma;
    u16 blksize, blkcnt, xfer, clock, nisen, eisen, nissig, eissig;
    u8 host1, timeout, clocked;
};

struct sdslot {
    struct blkdev bd;
    struct pcidev pci;          /* vendor 0: an ACPI device */
    u8 *r;
    u32 phys;
    u8 version;                 /* SDHCI spec version - 1 */
    u8 adma;
    u8 registered;
    u8 card;                    /* card initialized */
    u8 hc;                      /* block addressing (SDHC/SDXC, eMMC sector mode) */
    u8 fixed;                   /* non-removable: a fixed disk */
    u8 mmc;                     /* MMC / eMMC */
    u8 borrow;                  /* the BIOS uses the card too */
    u8 sel;                     /* borrow: the BIOS leaves the card selected */
    u8 cdbits;                  /* H1_CDTEST on fixed slots */
    u8 cmd23;                   /* SET_BLOCK_COUNT before multiple block transfers */
    u8 lost;                    /* the card stopped answering: set up again */
    u8 slot;
    u16 rca;
    u32 baseclk;                /* kHz */
    u32 cid[4];
    u32 *desc;                  /* ADMA2 descriptor table */
    u32 lastpoll;
    int no;
    struct sdsave sv;
    char hid[9], path[5];       /* ACPI */
    u32 uid, irq;
    u32 pnpkey;                 /* Windows 95/98/ME devnode of an ACPI host: key + 1, 0 none */
};

static struct sdslot *slots[MAXHOST];
static int nslot;
static u8 *xbuf;                /* 2 x 512 bytes: EXT_CSD */

static u8 rd8(struct sdslot *s, int o) { return RD8(s->r + o); }
static u16 rd16(struct sdslot *s, int o) { return RD16(s->r + o); }
static u32 rd32(struct sdslot *s, int o) { return RD32(s->r + o); }
static void wr8(struct sdslot *s, int o, u8 v) { WR8(s->r + o, v); }
static void wr16(struct sdslot *s, int o, u16 v) { WR16(s->r + o, v); }
static void wr32(struct sdslot *s, int o, u32 v) { WR32(s->r + o, v); }

static void host1(struct sdslot *s, u8 v)
{
    wr8(s, R_HOST1, (u8)(v | s->cdbits));
}

static int sd_reset(struct sdslot *s, u8 what)
{
    u32 t0 = plat_ms();
    wr8(s, R_RESET, what);
    while (rd8(s, R_RESET) & what) {
        if (plat_ms() - t0 > 200) return -1;
        cpu_pause();
    }
    return 0;
}

static int set_clock(struct sdslot *s, u32 khz)
{
    u32 div = 0, t0;
    u16 v;
    wr16(s, R_CLOCK, 0);
    if (!khz) return 0;
    if (s->version >= 2) {
        /* 10-bit divided clock: base / (2 * div), div = 0 means base */
        if (s->baseclk > khz) {
            div = (s->baseclk + 2 * khz - 1) / (2 * khz);
            if (div > 0x3FF) div = 0x3FF;
        }
        v = (u16)(((div & 0xFF) << 8) | ((div >> 8) & 3) << 6);
    } else {
        /* power-of-two divider: base / (2 * div) with div = 1, 2, 4 ... 128 */
        for (div = 1; div < 256 && s->baseclk / (2 * div) > khz; div <<= 1)
            ;
        if (s->baseclk <= khz) div = 0;
        v = (u16)((div & 0xFF) << 8);
    }
    wr16(s, R_CLOCK, (u16)(v | 1));                 /* internal clock on */
    t0 = plat_ms();
    while (!(rd16(s, R_CLOCK) & 2)) {
        if (plat_ms() - t0 > 100) return -1;
        cpu_pause();
    }
    wr16(s, R_CLOCK, (u16)(v | 1 | 4));             /* card clock on */
    mdelay(1);
    return 0;
}

/* the SD clock the clock control register selects, kHz */
static u32 clock_khz(struct sdslot *s, u16 v)
{
    u32 div = (v >> 8) & 0xFF;
    if (s->version >= 2) div |= (u32)((v >> 6) & 3) << 8;
    return div ? s->baseclk / (2 * div) : s->baseclk;
}

static void set_power(struct sdslot *s, int on)
{
    u32 caps = rd32(s, R_CAPS);
    u8 v = 0;
    if (on) {
        if (caps & (1UL << 24)) v = 0x0E;           /* 3.3 V */
        else if (caps & (1UL << 25)) v = 0x0C;      /* 3.0 V */
        else v = 0x0A;                              /* 1.8 V */
        wr8(s, R_POWER, v);
        wr8(s, R_POWER, (u8)(v | 1));
        mdelay(10);
    } else {
        wr8(s, R_POWER, 0);
    }
}

/* one command; data transfers are set up by the caller. 0 ok, -1 error / timeout */
static int sd_cmd(struct sdslot *s, u8 idx, u32 arg, u8 flags, u16 xfer)
{
    u32 t0 = plat_ms(), inh = ST_CMDINH | ((flags & DATA) || (flags & 3) == 3 ? ST_DATINH : 0);
    u16 st;
    while (rd32(s, R_STATE) & inh) {
        if (plat_ms() - t0 > 500) {
            sd_reset(s, 0x06);
            return -1;
        }
        cpu_pause();
    }
    wr16(s, R_NIS, 0xFFFF);
    wr16(s, R_EIS, 0xFFFF);
    wr32(s, R_ARG, arg);
    wr16(s, R_XFER, xfer);
    wr16(s, R_CMD, (u16)((idx << 8) | flags));
    t0 = plat_ms();
    for (;;) {
        st = rd16(s, R_NIS);
        if (st & NI_ERR) {
            wr16(s, R_EIS, 0xFFFF);
            sd_reset(s, 0x06);                      /* CMD and DAT lines */
            return -1;
        }
        if (st & NI_CMD) break;
        if (plat_ms() - t0 > 1000) {
            sd_reset(s, 0x06);
            return -1;
        }
        cpu_pause();
    }
    wr16(s, R_NIS, NI_CMD);
    if ((flags & 3) == 3 && !(flags & DATA)) {
        /* R1b: wait for the end of busy */
        t0 = plat_ms();
        while (!(rd16(s, R_NIS) & (NI_XFER | NI_ERR)) && (rd32(s, R_STATE) & ST_DATINH)) {
            if (plat_ms() - t0 > 3000) break;
            cpu_pause();
        }
        wr16(s, R_NIS, NI_XFER);
    }
    return 0;
}

static int app_cmd(struct sdslot *s, u8 idx, u32 arg, u8 flags)
{
    if (sd_cmd(s, 55, (u32)s->rca << 16, RSP_48, 0)) return -1;
    return sd_cmd(s, idx, arg, flags, 0);
}

static int card_status(struct sdslot *s, u32 *st)
{
    if (sd_cmd(s, 13, (u32)s->rca << 16, RSP_48, 0)) return -1;
    *st = rd32(s, R_RESP);
    return 0;
}

/* until the card is in the transfer state and ready for data (programming done) */
static int wait_ready(struct sdslot *s, u32 ms)
{
    u32 t0 = plat_ms(), st;
    for (;;) {
        if (card_status(s, &st) == 0 && ((st >> 9) & 0xF) == CS_TRAN && (st & 0x100)) return 0;
        if (plat_ms() - t0 > ms) return -1;
        udelay(50);
    }
}

/* waits for the end of a data transfer */
static int wait_xfer(struct sdslot *s, u32 ms)
{
    u32 t0 = plat_ms();
    u16 st;
    for (;;) {
        st = rd16(s, R_NIS);
        if (st & NI_ERR) {
            kprintf("sd%d: data error %04x adma %02x\n", s->no, rd16(s, R_EIS), rd8(s, R_ADMAERR));
            wr16(s, R_EIS, 0xFFFF);
            sd_reset(s, 0x06);
            return -1;
        }
        if (st & NI_XFER) break;
        if (plat_ms() - t0 > ms) {
            sd_reset(s, 0x06);
            return -1;
        }
        cpu_pause();
    }
    wr16(s, R_NIS, 0xFFFF);
    return 0;
}

/* PIO block transfer through the buffer data port */
static int pio_blocks(struct sdslot *s, u8 *buf, u32 n, int wr)
{
    u32 i, k, t0;
    u32 *p = (u32 *)buf;
    u16 want = wr ? NI_BWR : NI_BRD;
    for (i = 0; i < n; i++) {
        t0 = plat_ms();
        while (!(rd16(s, R_NIS) & (want | NI_ERR))) {
            if (plat_ms() - t0 > 1000) return -1;
            cpu_pause();
        }
        if (rd16(s, R_NIS) & NI_ERR) return -1;
        wr16(s, R_NIS, want);
        for (k = 0; k < 128; k++) {
            if (wr) wr32(s, R_DATA, *p++);
            else *p++ = rd32(s, R_DATA);
        }
    }
    return 0;
}

/* a command with cnt 512-byte data blocks (buf in pool memory); acmd12: stop a multiple
   block transfer with Auto CMD12 (else the block count was set with CMD23) */
static int data_cmd(struct sdslot *s, u8 idx, u32 arg, void *buf, u32 cnt, int wr, int acmd12)
{
    u16 xfer;
    wr16(s, R_BLKSIZE, 512 | (7 << 12));            /* 512 bytes, SDMA boundary 512K */
    wr16(s, R_BLKCNT, (u16)cnt);
    xfer = (u16)((wr ? 0 : 0x10) | (cnt > 1 ? 0x22 : 0));     /* read, multi block + block count */
    if (cnt > 1 && acmd12) xfer |= 0x04;            /* auto CMD12 */
    if (s->adma) {
        u32 p = vphys(buf), left = cnt * 512, i = 0;
        while (left && i < MAXADMA) {
            u32 len = MIN(left, 0x8000UL);          /* 32K per descriptor, no 64K wrap issues */
            s->desc[i * 2] = (len << 16) | 0x21;    /* valid, transfer */
            s->desc[i * 2 + 1] = p;
            p += len;
            left -= len;
            i++;
        }
        if (left) return -1;
        s->desc[(i - 1) * 2] |= 0x02;               /* end */
        wr32(s, R_ADMA, vphys(s->desc));
        wr8(s, R_HOST1, (u8)((rd8(s, R_HOST1) & ~H1_DMA) | 0x10));     /* ADMA2 32-bit */
        xfer |= 0x01;                               /* DMA */
    }
    if (sd_cmd(s, idx, arg, RSP_48 | DATA, xfer)) return -1;
    if (!s->adma && pio_blocks(s, buf, cnt, wr)) {
        sd_reset(s, 0x06);
        return -1;
    }
    return wait_xfer(s, 5000 + cnt * 20);
}

/* ---- borrow mode: the BIOS's controller state around our commands ---- */

static void borrow_begin(struct sdslot *s)
{
    struct sdsave *v = &s->sv;
    if (!s->borrow) return;
    /* the SDMA address is neither used nor written: a write restarts an SDMA transfer */
    v->host1 = rd8(s, R_HOST1);
    v->timeout = rd8(s, R_TIMEOUT);
    v->blksize = rd16(s, R_BLKSIZE);
    v->blkcnt = rd16(s, R_BLKCNT);
    v->xfer = rd16(s, R_XFER);
    v->clock = rd16(s, R_CLOCK);
    v->nisen = rd16(s, R_NISEN);
    v->eisen = rd16(s, R_EISEN);
    v->nissig = rd16(s, R_NISSIG);
    v->eissig = rd16(s, R_EISSIG);
    v->adma = rd32(s, R_ADMA);
    wr16(s, R_NISSIG, 0);                           /* no interrupts from our commands */
    wr16(s, R_EISSIG, 0);
    wr16(s, R_NISEN, 0xFFFF);
    wr16(s, R_EISEN, 0xFFFF);
    wr8(s, R_TIMEOUT, 0x0E);
    /* a slow BIOS clock: default speed (20 MHz, valid for SD and MMC) while we work */
    v->clocked = 0;
    if (!(v->host1 & H1_HS) && clock_khz(s, v->clock) < 15000) {
        set_clock(s, 20000);
        v->clocked = 1;
    }
    if (!s->sel) sd_cmd(s, 7, (u32)s->rca << 16, RSP_48B, 0);
}

static void borrow_end(struct sdslot *s)
{
    struct sdsave *v = &s->sv;
    u32 t0;
    if (!s->borrow) return;
    if (!s->sel) sd_cmd(s, 7, 0, RSP_NONE, 0);      /* deselect again, as the BIOS had it */
    t0 = plat_ms();
    while ((rd32(s, R_STATE) & (ST_CMDINH | ST_DATINH)) && plat_ms() - t0 < 100) cpu_pause();
    if (v->clocked) {
        wr16(s, R_CLOCK, (u16)((v->clock & ~4) | 1));
        t0 = plat_ms();
        while (!(rd16(s, R_CLOCK) & 2) && plat_ms() - t0 < 100) cpu_pause();
        wr16(s, R_CLOCK, v->clock);
    }
    wr8(s, R_HOST1, v->host1);
    wr8(s, R_TIMEOUT, v->timeout);
    wr16(s, R_BLKSIZE, v->blksize);
    wr16(s, R_BLKCNT, v->blkcnt);
    wr16(s, R_XFER, v->xfer);
    wr32(s, R_ADMA, v->adma);
    wr16(s, R_NIS, 0xFFFF);
    wr16(s, R_EIS, 0xFFFF);
    wr16(s, R_NISEN, v->nisen);
    wr16(s, R_EISEN, v->eisen);
    wr16(s, R_EISSIG, v->eissig);
    wr16(s, R_NISSIG, v->nissig);
}

/* ---- block device ---- */

static int card_init(struct sdslot *s);

/* after a failed command: card back to the transfer state, or set up again next time */
static void recover(struct sdslot *s)
{
    u32 st;
    sd_reset(s, 0x06);
    if (card_status(s, &st)) {
        s->lost = 1;
        return;
    }
    st = (st >> 9) & 0xF;
    if (st == CS_DATA || st == CS_RCV) sd_cmd(s, 12, 0, RSP_48B, 0);      /* STOP_TRANSMISSION */
    if (wait_ready(s, 1000)) s->lost = 1;
}

static int transfer(struct sdslot *s, u32 lba, u32 cnt, void *buf, int wr)
{
    u32 addr = s->hc ? lba : lba * 512;
    int r;
    if (cnt > 1 && s->cmd23) {
        /* SET_BLOCK_COUNT, then READ / WRITE_MULTIPLE_BLOCK without a stop command */
        if (sd_cmd(s, 23, cnt, RSP_48, 0)) return -1;
        r = data_cmd(s, (u8)(wr ? 25 : 18), addr, buf, cnt, wr, 0);
    } else if (wr) {
        r = data_cmd(s, (u8)(cnt > 1 ? 25 : 24), addr, buf, cnt, wr, 1);
    } else {
        r = data_cmd(s, (u8)(cnt > 1 ? 18 : 17), addr, buf, cnt, wr, 1);
    }
    /* writes: until the card has programmed the data */
    if (!r && wr) r = wait_ready(s, 3000);
    return r;
}

static int sd_rw(struct blkdev *d, u64 lba, u32 cnt, void *buf, int wr)
{
    struct sdslot *s = d->priv;
    int r;
    if (!s->card) return -2;
    if (lba + cnt > d->sectors) return -1;      /* SD / MMC addresses are 32 bits */
    if (!s->fixed && !(rd32(s, R_STATE) & ST_CARD)) return -2;
    if (wr && d->wprot) return -3;
    if (wr && !s->fixed && !(rd32(s, R_STATE) & ST_WP)) return -3;     /* WP pin: 0 = protected */
    if (s->lost && !s->borrow) {
        kprintf("sd%d: card set up again\n", s->no);
        if (card_init(s)) return -1;
    }
    borrow_begin(s);
    r = transfer(s, (u32)lba, cnt, buf, wr);
    if (r) recover(s);
    borrow_end(s);
    return r;
}

static int sd_flush(struct blkdev *d)
{
    (void)d;
    return 0;
}

static int sd_ready(struct blkdev *d)
{
    struct sdslot *s = d->priv;
    if (!s->card) return -2;
    return s->fixed || (rd32(s, R_STATE) & ST_CARD) ? 0 : -2;
}

static struct blkops sd_ops = { sd_rw, sd_flush, sd_ready };

/* ---- card registers ---- */

static u32 bits(const u32 *r, int hi, int lo)
{
    /* CSD/CID bit field; r[] holds bits 127..8 shifted (R2 without CRC): bit n is at n - 8 */
    u32 v = 0;
    int b;
    for (b = hi; b >= lo; b--) {
        int k = b - 8;
        v = (v << 1) | ((r[k / 32] >> (k % 32)) & 1);
    }
    return v;
}

static int read_r2(struct sdslot *s, u8 idx, u32 arg, u32 *out)
{
    int i;
    if (sd_cmd(s, idx, arg, RSP_136, 0)) return -1;
    for (i = 0; i < 4; i++) out[i] = rd32(s, R_RESP + i * 4);
    return 0;
}

/* capacity in 512-byte blocks from the CSD, or from EXT_CSD SEC_COUNT of an MMC device that
   has one (all over 2 GB, and some of 2 GB in byte mode, whose CSD cannot tell the size) */
static u64 capacity(struct sdslot *s, const u32 *csd, const u8 *ext)
{
    u64 blocks;
    if (s->mmc && ext && *(const u32 *)(ext + 212) && (s->hc || bits(csd, 73, 62) == 0xFFF))
        return *(const u32 *)(ext + 212);
    if (!s->mmc && bits(csd, 127, 126) == 1) {
        /* SD CSD 2.0: (C_SIZE + 1) * 512K */
        return (u64)(bits(csd, 69, 48) + 1) * 1024;
    }
    {
        u32 csize = bits(csd, 73, 62), mult = bits(csd, 49, 47), bl = bits(csd, 83, 80);
        blocks = (u64)(csize + 1) << (mult + 2);
        blocks = (blocks << bl) / 512;
    }
    return blocks;
}

static int read_ext(struct sdslot *s, u8 *buf)
{
    return data_cmd(s, 8, 0, buf, 1, 0, 0);         /* SEND_EXT_CSD */
}

/* CMD6 SWITCH of an EXT_CSD byte (mode 1 set bits, 2 clear bits, 3 write); check: the
   card's status afterwards */
static int mmc_switch(struct sdslot *s, u8 index, u8 value, u8 mode, int check)
{
    u32 st;
    if (sd_cmd(s, 6, ((u32)mode << 24) | ((u32)index << 16) | ((u32)value << 8), RSP_48B, 0)) return -1;
    if (!check) return 0;
    if (wait_ready(s, 1000) || card_status(s, &st) || (st & 0x80)) return -1;     /* SWITCH_ERROR */
    return 0;
}

/* identity: the model string, serial number and keys from the CID */
static void identity(struct sdslot *s)
{
    char *m = s->bd.model, name[8], sz[16];
    u32 psn, mb = (u32)(s->bd.sectors >> 11);
    int i, k = 0;
    static const char hex[] = "0123456789ABCDEF";
    /* product name: SD 5 characters (bits 103:64), MMC 6 (bits 103:56) */
    for (i = 103; i >= (s->mmc ? 56 : 64) && k < 6; i -= 8) {
        char c = (char)bits(s->cid, i, i - 7);
        name[k++] = (c > 32 && c < 127) ? c : ' ';
    }
    name[k] = 0;
    trim_string(name);
    if (mb >= 1024) ksnprintf(sz, sizeof(sz), "%lu GB", (mb + 512) / 1024);
    else ksnprintf(sz, sizeof(sz), "%lu MB", mb);
    if (s->mmc && s->fixed) ksnprintf(m, sizeof(s->bd.model), "eMMC %s %s", name, sz);
    else if (s->mmc) ksnprintf(m, sizeof(s->bd.model), "MMC card %s", name);
    else ksnprintf(m, sizeof(s->bd.model), "SD card %s", name);
    trim_string(m);
    psn = s->mmc ? bits(s->cid, 47, 16) : bits(s->cid, 55, 24);
    for (i = 0; i < 8; i++) s->bd.serial[i] = hex[(psn >> (28 - 4 * i)) & 15];
    s->bd.serial[8] = 0;
    s->bd.kind = (u8)(s->mmc ? DK_MMC : DK_SD);
    if (s->pci.vendor) s->bd.key = 0x53440000UL | (PCI_BDF(&s->pci) & 0xFFFF);
    else if (s->pnpkey) s->bd.key = 0x53440000UL | ((s->pnpkey - 1) & 0xFFFF);
    else s->bd.key = 0x4D4D0000UL | ((s->phys >> 12) & 0xFFFF);
    s->bd.key2 = crc32(psn, s->cid, sizeof(s->cid));
}

/* SD: ACMD41 until powered up. 0 ok, -1 no SD card, -2 no power up */
static int sd_opcond(struct sdslot *s, int v2, u32 *ocr)
{
    u32 t0 = plat_ms();
    for (;;) {
        if (app_cmd(s, 41, 0x00FF8000UL | (v2 ? 0x40000000UL : 0), RSP_R3)) return -1;
        *ocr = rd32(s, R_RESP);
        if (*ocr & 0x80000000UL) return 0;
        if (plat_ms() - t0 > 1500) {
            kprintf("sd%d: card did not power up (OCR %08lx)\n", s->no, *ocr);
            return -2;
        }
        mdelay(10);
    }
}

/* MMC: CMD1 with the device's voltage window and sector mode. 0 ok, -1 no MMC, -2 no power up */
static int mmc_opcond(struct sdslot *s, u32 *ocr)
{
    u32 t0 = plat_ms(), arg;
    if (sd_cmd(s, 1, 0, RSP_R3, 0)) return -1;      /* inquiry: the voltage window */
    arg = 0x40000000UL | (rd32(s, R_RESP) & 0x00FF8080UL);
    if (!(arg & 0x00FF8080UL)) arg |= 0x00FF8080UL;
    for (;;) {
        if (sd_cmd(s, 1, arg, RSP_R3, 0)) return -2;
        *ocr = rd32(s, R_RESP);
        if (*ocr & 0x80000000UL) return 0;
        if (plat_ms() - t0 > 1500) {
            kprintf("sd%d: MMC device did not power up (OCR %08lx)\n", s->no, *ocr);
            return -2;
        }
        mdelay(5);
    }
}

/* MMC bus width: 8 then 4 bits, checked by reading EXT_CSD again */
static void mmc_width(struct sdslot *s, const u8 *ext)
{
    u8 *chk = xbuf + 512;
    int i;
    for (i = 0; i < 2; i++) {
        u8 w = (u8)(i ? 1 : 2);                     /* EXT_CSD BUS_WIDTH: 1 = 4 bits, 2 = 8 bits */
        if (w == 2 && (s->version < 2 || !(rd32(s, R_CAPS) & CAP_8BIT))) continue;
        if (mmc_switch(s, 183, w, 3, 1)) continue;
        host1(s, (u8)((rd8(s, R_HOST1) & ~(H1_4BIT | H1_8BIT | H1_CDTEST)) | (w == 2 ? H1_8BIT : H1_4BIT)));
        if (read_ext(s, chk) == 0 && !memcmp(chk + 212, ext + 212, 4) && chk[192] == ext[192] &&
            chk[196] == ext[196])
            return;
        kprintf("sd%d: %d-bit bus does not work\n", s->no, w == 2 ? 8 : 4);
        mmc_switch(s, 183, 0, 3, 0);
        host1(s, (u8)(rd8(s, R_HOST1) & ~(H1_4BIT | H1_8BIT | H1_CDTEST)));
        wait_ready(s, 500);
    }
}

/* MMC / eMMC after selection: user data area, high speed timing, bus width */
static int mmc_setup(struct sdslot *s, const u32 *csd, u8 **extp)
{
    u8 *ext = xbuf;
    u32 vers = bits(csd, 125, 122), type, khz = 20000, st;
    set_clock(s, 20000);
    s->cmd23 = (u8)(vers >= 3);
    *extp = NULL;
    if (vers < 4 || read_ext(s, ext)) {
        if (s->hc) {
            kprintf("sd%d: no EXT_CSD from a sector mode MMC device\n", s->no);
            return -1;
        }
        return 0;
    }
    *extp = ext;
    if (ext[179] & 7) {
        /* PARTITION_ACCESS: the user data area, not a boot or RPMB partition */
        if (mmc_switch(s, 179, 7, 2, 1)) {
            kprintf("sd%d: user data area cannot be selected (PARTITION_CONFIG %02x)\n", s->no, ext[179]);
            return -1;
        }
        ext[179] &= ~7;
    }
    type = ext[196];
    if ((type & 3) && (rd32(s, R_CAPS) & CAP_HS) && mmc_switch(s, 185, 1, 3, 0) == 0) {
        /* HS_TIMING: high speed, 52 or 26 MHz */
        khz = (type & 2) ? 52000 : 26000;
        host1(s, (u8)((rd8(s, R_HOST1) & ~H1_CDTEST) | H1_HS));
        set_clock(s, khz);
        if (wait_ready(s, 1000) || card_status(s, &st) || (st & 0x80)) {
            kprintf("sd%d: high speed timing failed\n", s->no);
            khz = 20000;
            host1(s, (u8)(rd8(s, R_HOST1) & ~(H1_HS | H1_CDTEST)));
            set_clock(s, khz);
            mmc_switch(s, 185, 0, 3, 1);
        }
    }
    mmc_width(s, ext);
    DBG("sd%d: EXT_CSD rev %u, card type %02x, %lu kHz, bus width %u\n", s->no, ext[192], ext[196], khz,
        (rd8(s, R_HOST1) & H1_8BIT) ? 8 : (rd8(s, R_HOST1) & H1_4BIT) ? 4 : 1);
    return 0;
}

static const char *bus_desc(struct sdslot *s)
{
    u8 h = rd8(s, R_HOST1);
    if (h & H1_8BIT) return h & H1_HS ? "8-bit high speed" : "8-bit";
    if (h & H1_4BIT) return h & H1_HS ? "4-bit high speed" : "4-bit";
    return "1-bit";
}

static void card_done(struct sdslot *s, const u32 *csd, const u8 *ext)
{
    if (s->mmc && bits(s->cid, 113, 112) && bits(s->cid, 113, 112) < 3) s->fixed = 1;   /* BGA / POP: soldered */
    s->bd.removable = (u8)!s->fixed;
    s->bd.sectors = capacity(s, csd, ext);
    s->bd.present = 1;
    /* write protection: the slot's switch (removable cards), the CSD's permanent and
       temporary write protection */
    s->bd.wprot = (u8)((!s->fixed && !(rd32(s, R_STATE) & ST_WP)) || bits(csd, 13, 12));
    identity(s);
    s->card = 1;
    s->lost = 0;
    kprintf("sd%d: %s, %s, %lu MB, %s%s%s%s%s\n", s->no, s->bd.model,
            s->mmc ? (s->hc ? "sector mode" : "byte mode") : (s->hc ? "SDHC/SDXC" : "SDSC"),
            (u32)(s->bd.sectors >> 11), bus_desc(s), s->adma ? ", ADMA2" : ", PIO",
            s->fixed ? ", fixed disk" : "", s->borrow ? ", shared with the BIOS" : "",
            s->bd.wprot ? ", write protected" : "");
}

/* card identification and setup; 0 ok */
static int card_init(struct sdslot *s)
{
    u32 ocr = 0, csd[4];
    u8 *ext = NULL;
    int v2, r;
    s->card = 0;
    s->rca = 0;
    s->mmc = 0;
    s->cmd23 = 0;
    sd_reset(s, 0x06);
    set_power(s, 1);
    host1(s, 0);
    if (set_clock(s, 400)) return -1;
    wr8(s, R_TIMEOUT, 0x0E);
    mdelay(2);
    sd_cmd(s, 0, 0, RSP_NONE, 0);                   /* GO_IDLE_STATE */
    v2 = sd_cmd(s, 8, 0x1AA, RSP_48, 0) == 0 && (rd32(s, R_RESP) & 0xFFF) == 0x1AA;
    r = sd_opcond(s, v2, &ocr);
    if (r == -1) {
        /* no SD card: an MMC card or eMMC device? */
        sd_cmd(s, 0, 0, RSP_NONE, 0);
        mdelay(1);
        r = mmc_opcond(s, &ocr);
        if (r == -1) {
            kprintf("sd%d: no memory card (SDIO card or empty slot)\n", s->no);
            return -1;
        }
        s->mmc = 1;
    }
    if (r) return -1;
    s->hc = (u8)(s->mmc ? (ocr & 0x60000000UL) == 0x40000000UL : (ocr & 0x40000000UL) != 0);
    if (read_r2(s, 2, 0, s->cid)) return -1;        /* ALL_SEND_CID */
    if (s->mmc) {
        s->rca = 1;                                 /* SET_RELATIVE_ADDR */
        if (sd_cmd(s, 3, (u32)s->rca << 16, RSP_48, 0)) return -1;
    } else {
        if (sd_cmd(s, 3, 0, RSP_48, 0)) return -1;  /* SEND_RELATIVE_ADDR */
        s->rca = (u16)(rd32(s, R_RESP) >> 16);
    }
    if (read_r2(s, 9, (u32)s->rca << 16, csd)) return -1;          /* SEND_CSD */
    if (sd_cmd(s, 7, (u32)s->rca << 16, RSP_48B, 0)) return -1;     /* SELECT_CARD */
    if (s->mmc) {
        if (mmc_setup(s, csd, &ext)) return -1;
    } else {
        /* 4-bit bus, 25 MHz */
        if (app_cmd(s, 6, 2, RSP_48) == 0) host1(s, (u8)((rd8(s, R_HOST1) & ~H1_CDTEST) | H1_4BIT));
        set_clock(s, 25000);
    }
    if (!s->hc) sd_cmd(s, 16, 512, RSP_48, 0);      /* SET_BLOCKLEN */
    card_done(s, csd, ext);
    return 0;
}

/* The BIOS has the card running (INT 13h disk): find its address (a card in the stand-by
   or transfer state answers SEND_STATUS at its RCA only), read CID / CSD / EXT_CSD and keep
   everything else as the BIOS set it. 0 when the card is used in borrow mode. */
static int adopt(struct sdslot *s)
{
    u32 csd[4], st = 0, rca, t0 = plat_ms();
    u8 *ext = xbuf;
    int found = 0;
    s->borrow = 1;
    s->sel = 1;
    s->rca = 0;
    borrow_begin(s);
    for (rca = 1; rca <= 0xFFFF && !found; rca++) {
        if (sd_cmd(s, 13, rca << 16, RSP_48, 0) == 0) {
            st = (rd32(s, R_RESP) >> 9) & 0xF;
            found = 1;
            s->rca = (u16)rca;
        }
        if (plat_ms() - t0 > 15000) break;
    }
    if (!found || (st != CS_STBY && st != CS_TRAN)) {
        if (found) kprintf("sd%d: card of the BIOS in state %lu, not used together\n", s->no, st);
        borrow_end(s);
        s->borrow = 0;
        return -1;
    }
    DBG("sd%d: BIOS card at RCA %04x (state %lu), found in %lu ms\n", s->no, s->rca, st, plat_ms() - t0);
    if (st == CS_TRAN) sd_cmd(s, 7, 0, RSP_NONE, 0);               /* to stand-by for CID / CSD */
    s->sel = (u8)(st == CS_TRAN);
    if (read_r2(s, 10, (u32)s->rca << 16, s->cid) || read_r2(s, 9, (u32)s->rca << 16, csd) ||
        sd_cmd(s, 7, (u32)s->rca << 16, RSP_48B, 0)) {
        kprintf("sd%d: card of the BIOS does not answer\n", s->no);
        borrow_end(s);
        s->borrow = 0;
        return -1;
    }
    /* SEND_EXT_CSD answers on MMC devices only (CMD8 is SEND_IF_COND for SD cards) */
    s->mmc = (u8)(read_ext(s, ext) == 0);
    if (!s->mmc) {
        ext = NULL;
        sd_reset(s, 0x06);
        s->hc = (u8)(bits(csd, 127, 126) != 0);
    } else {
        /* sector addressing is for devices over 2 GB only (their CSD has C_SIZE FFFh and
           EXT_CSD SEC_COUNT the size); a device of 2 GB or less stays in byte mode */
        s->hc = (u8)(bits(csd, 73, 62) == 0xFFF &&
                     (ext[212] | ((u32)ext[213] << 8) | ((u32)ext[214] << 16) | ((u32)ext[215] << 24)) > 0x400000UL);
        s->cmd23 = (u8)(bits(csd, 125, 122) >= 3);
        if (ext[179] & 7) {
            kprintf("sd%d: the BIOS has a boot partition selected, not used\n", s->no);
            borrow_end(s);
            s->borrow = 0;
            return -1;
        }
    }
    card_done(s, csd, ext);
    borrow_end(s);
    return 0;
}

/* SD clock and power on, a card in the transfer or stand-by state: the BIOS uses this slot */
static int bios_active(struct sdslot *s)
{
    u16 c = rd16(s, R_CLOCK);
    return (rd8(s, R_POWER) & 1) && (c & 4) && (c & 2) && (s->fixed || (rd32(s, R_STATE) & ST_CARD));
}

/* controller registers for our own use: reset, status bits on, no interrupt signals */
static int own(struct sdslot *s)
{
    s->borrow = 0;
    if (sd_reset(s, 0x01)) {
        kprintf("sd%d: controller reset failed\n", s->no);
        return -1;
    }
    wr16(s, R_NISEN, 0xFFFF);
    wr16(s, R_EISEN, 0xFFFF);
    wr16(s, R_NISSIG, 0);
    wr16(s, R_EISSIG, 0);
    /* fixed slots may have no card detect line: "card inserted" from the test level */
    if (s->fixed && s->version >= 1) s->cdbits = H1_CDTEST;
    host1(s, 0);
    {
        /* card detect needs a moment after the reset */
        u32 t0 = plat_ms();
        while (!(rd32(s, R_STATE) & ST_STABLE) && plat_ms() - t0 < 100) cpu_pause();
    }
    return 0;
}

/* card inserted or removed since the last look */
static void slot_check(struct sdslot *s)
{
    int in = (rd32(s, R_STATE) & ST_CARD) != 0;
    if (in && !s->registered) {
        if (s->borrow) own(s);
        if (card_init(s) == 0) {
            blk_register(&s->bd);
            s->registered = 1;
        }
    } else if (!in && s->registered) {
        kprintf("sd%d: card removed\n", s->no);
        s->card = 0;
        s->bd.present = 0;
        blk_unregister(&s->bd);
        s->registered = 0;
        if (s->borrow) own(s);
        set_power(s, 0);
    }
}

/* called when DOS asks a slot drive for its medium: card changes */
static void sd_poll(void)
{
    int i;
    u32 now = plat_ms();
    for (i = 0; i < nslot; i++) {
        struct sdslot *s = slots[i];
        if (s->fixed || !s->r || now - s->lastpoll < 300 || (s->borrow && (core_flags & CF_PNP))) continue;
        s->lastpoll = now;
        slot_check(s);
    }
}

void sdhci_poll(void)
{
    sd_poll();
}

/* the first look at a slot: the BIOS's card, else our own initialization */
static void slot_start(struct sdslot *s)
{
    if (bios_active(s) && adopt(s) == 0) {
        blk_register(&s->bd);
        s->registered = 1;
    } else {
        if (own(s)) return;
        s->lastpoll = plat_ms();
        if (s->fixed) {
            if (card_init(s) == 0) {
                blk_register(&s->bd);
                s->registered = 1;
            }
        } else {
            slot_check(s);
        }
        if (!s->registered) kprintf("sd%d: no card\n", s->no);
    }
    if (!s->fixed) blk_poll_hook = sd_poll;
}

/* a slot at physical address phys: of PCI function p, or of an ACPI device (p NULL) */
static struct sdslot *add_slot(u32 phys, const struct pcidev *p, int k, int fixed, const char *hid,
                               const char *path, u32 uid, u32 irq, u32 pnpkey)
{
    struct sdslot *s;
    u32 caps;
    int i;
    if (nslot >= MAXHOST) return NULL;
    for (i = 0; i < nslot; i++)
        if (slots[i]->phys == phys) return NULL;    /* the same controller (PCI and ACPI) */
    if (!xbuf) xbuf = kmalloc_align(1024, 512);
    s = kmalloc(sizeof(*s));
    if (!s || !xbuf) return NULL;
    if (p) s->pci = *p;
    s->phys = phys;
    s->r = plat_mmio(phys, 0x100);
    if (!s->r || rd16(s, R_VERSION) == 0xFFFF) {
        kprintf("sdhci: registers at %08lx do not answer\n", phys);
        kfree(s);
        return NULL;
    }
    s->no = nslot;
    s->slot = (u8)k;
    if (hid) strlcpy(s->hid, hid, sizeof(s->hid));
    if (path) strlcpy(s->path, path, sizeof(s->path));
    s->uid = uid;
    s->irq = p ? p->irq : irq;
    s->pnpkey = pnpkey;
    s->version = (u8)(rd16(s, R_VERSION) & 0xFF);
    caps = rd32(s, R_CAPS);
    s->baseclk = ((caps >> 8) & (s->version >= 2 ? 0xFF : 0x3F)) * 1000UL;
    if (!s->baseclk) s->baseclk = 50000;
    s->adma = (caps & CAP_ADMA2) != 0;
    if (s->adma) s->desc = kmalloc_align(MAXADMA * 8, 8);
    if (s->adma && !s->desc) s->adma = 0;
    /* slot type (SDHCI 3.0): embedded slot or shared bus */
    s->fixed = (u8)(fixed || (s->version >= 2 && (caps >> 30) != 0));
    s->bd.ops = &sd_ops;
    s->bd.priv = s;
    s->bd.kind = DK_SD;
    s->bd.removable = (u8)!s->fixed;
    s->bd.ssize = 512;
    s->bd.maxxfer = s->adma ? MIN(BOUNCE_SIZE / 512, MAXADMA * 64) : 64;
    slots[nslot++] = s;
    if (p)
        kprintf("sdhci: %02x:%02x.%u %04x:%04x slot %d, spec %u.00, base clock %lu MHz%s%s\n", p->bus, p->dev,
                p->fn, p->vendor, p->device, k, s->version + 1, s->baseclk / 1000, s->adma ? ", ADMA2" : "",
                s->fixed ? ", non-removable" : "");
    else
        kprintf("sdhci: ACPI %s (%s, UID %lu) at %08lx IRQ %lu, spec %u.00, base clock %lu MHz%s%s\n", path, hid,
                uid, phys, irq, s->version + 1, s->baseclk / 1000, s->adma ? ", ADMA2" : "",
                s->fixed ? ", non-removable" : "");
    slot_start(s);
    return s;
}

/* Bay Trail: the OCP timeout of the SCC fabric off, as Linux does for these controllers
   (IOSF sideband port 63h register 1078h through the host bridge's MCR / MDR) */
static void byt_ocp(void)
{
    static int done;
    u32 v;
    if (done || pci_rd32(0, 0, 0, 0) != 0x0F008086UL) return;
    done = 1;
    pci_wr32(0, 0, 0, 0xD8, 0x00001000UL);                          /* MCRX: register 1078h & ~FFh */
    pci_wr32(0, 0, 0, 0xD0, (0x06UL << 24) | (0x63UL << 16) | (0x78UL << 8) | 0xF0);   /* read */
    v = pci_rd32(0, 0, 0, 0xD4);
    if (v == 0xFFFFFFFFUL || !(v & 0x700)) return;
    pci_wr32(0, 0, 0, 0xD4, v & ~0x700UL);
    pci_wr32(0, 0, 0, 0xD8, 0x00001000UL);
    pci_wr32(0, 0, 0, 0xD0, (0x07UL << 24) | (0x63UL << 16) | (0x78UL << 8) | 0xF0);   /* write */
    DBG("sdhci: Bay Trail OCP timeout %08lx -> %08lx\n", v, v & ~0x700UL);
}

/* Intel SD host controllers known by their PCI ids */
static int pci_type(const struct pcidev *p)
{
    static const u16 emmc[] = { 0x0F14, 0x0F50, 0x2294, 0x08E5, 0x08E6, 0x9D2B, 0x19DB, 0x18DB, 0x0ACC, 0x1ACC,
                                0x5ACC, 0x31CC, 0x9DC4, 0x34C4, 0x4B47, 0x02C4, 0x4DC4, 0x98C4, 0x54C4 };
    static const u16 sdio[] = { 0x0F15, 0x2295, 0x08F9, 0x08FA, 0x08FB, 0x9D2C, 0x0AD0, 0x1AD0, 0x5AD0, 0x31D0 };
    int i;
    if (p->vendor != 0x8086) return SDT_ANY;
    for (i = 0; i < (int)ARRAY_SIZE(emmc); i++)
        if (p->device == emmc[i]) return SDT_EMMC;
    for (i = 0; i < (int)ARRAY_SIZE(sdio); i++)
        if (p->device == sdio[i]) return SDT_SDIO;
    return SDT_ANY;
}

int sdhci_probe(struct pcidev *p)
{
    u8 slotinfo = pci_rd8(p->bus, p->dev, p->fn, 0x40);
    int n = ((slotinfo >> 4) & 7) + 1, first = slotinfo & 7, k, t = pci_type(p);
    if (t == SDT_SDIO) {
        kprintf("sdhci: %02x:%02x.%u %04x:%04x SDIO controller, not used\n", p->bus, p->dev, p->fn, p->vendor,
                p->device);
        return -1;
    }
    pci_enable(p, 1, 0, 1);
    if (p->vendor == 0x8086 && ((p->device & 0xFFF0) == 0x0F10 || p->device == 0x0F50)) byt_ocp();
    for (k = 0; k < n && first + k < 6; k++) {
        u32 phys = pci_bar_mem(p, first + k, NULL);
        if (phys) add_slot(phys, p, k, t == SDT_EMMC, NULL, NULL, 0, 0, 0);
    }
    return 0;
}

/* ---- controllers of the ACPI namespace (no PCI function) ---- */

static const struct { const char *hid; u8 uid, type; } acpi_ids[] = {
    { "80860F14", 1, SDT_EMMC }, { "80860F14", 2, SDT_SDIO }, { "80860F14", 3, SDT_SD },
    { "80860F16", 0, SDT_SD }, { "INT33BB", 2, SDT_SDIO }, { "INT33BB", 3, SDT_SD },
    { "80865ACA", 0, SDT_SD }, { "80865ACC", 0, SDT_EMMC }, { "80865AD0", 0, SDT_SDIO },
    { "INT33C6", 0, SDT_SDIO }, { "INT3436", 0, SDT_SDIO }, { "INT344D", 0, SDT_SDIO },
    { "AMDI0040", 0, SDT_EMMC }, { "AMDI0041", 0, SDT_EMMC }
};

static int acpi_type(const char *hid, u32 uid)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(acpi_ids); i++)
        if (!strncmp(hid, acpi_ids[i].hid, 9) && (!acpi_ids[i].uid || acpi_ids[i].uid == uid)) return acpi_ids[i].type;
    return SDT_ANY;
}

static int base_ok(u32 b)
{
    return b && b != 0xFFFFFFFFUL && !(b & 0xFF);
}

/* only: the device whose registers are at this address (0 all); pnpkey: see add_slot */
static int probe_acpi(u32 only, u32 pnpkey)
{
    static const char *const hids[] = { "80860F14", "80860F16", "INT33BB", "80865ACA", "80865ACC", "80865AD0",
                                        "INT33C6", "INT3436", "INT344D", "AMDI0040", "AMDI0041", "PNP0D40", NULL };
    struct acpi_dev ad[8];
    int n = acpi_find_devices(hids, ad, 8), i, j, k, nm, found = 0;
    for (i = 0; i < n; i++) {
        const char *path = ad[i].path;
        char hid[9];
        u32 uid = 0, mem[4], base[4], len[4], v, phys = 0, irq = 0, psat;
        int t, fixed;
        u8 *r;
        nm = acpi_dev_info(path, hid, &uid, mem, 4);
        if (nm < 0) continue;
        if (!hid[0]) strlcpy(hid, ad[i].hid, sizeof(hid));
        t = acpi_type(hid, uid);
        if (t == SDT_SDIO) {
            kprintf("sdhci: ACPI %s (%s, UID %lu): SDIO slot, not used\n", path, hid, uid);
            continue;
        }
        if (acpi_dev_const(path, "_STA", &v) == 0 && (v & 3) != 3) {
            kprintf("sdhci: ACPI %s (%s): disabled (_STA %lx)\n", path, hid, v);
            continue;
        }
        k = acpi_dev_mem(path, base, len, 4);
        for (j = 0; j < k && !phys; j++)
            if (base_ok(base[j])) phys = base[j];
        for (j = 0; j < nm && !phys; j++)
            if (base_ok(mem[j])) phys = mem[j];
        if (!phys) {
            kprintf("sdhci: ACPI %s (%s): no register address (disabled in the BIOS?)\n", path, hid);
            continue;
        }
        if (only && phys != only) continue;
        acpi_dev_irq(path, &irq);
        fixed = t == SDT_EMMC || (acpi_dev_const(path, "_RMV", &v) == 0 && !v) ||
                (acpi_dev_prop(path, "non-removable", &v) == 0 && v);
        /* a controller in D3: D0 through its power state register, as its _PS0 does (Atom LPSS) */
        r = plat_mmio(phys, 0x100);
        if (r && RD16(r + R_VERSION) == 0xFFFF && acpi_dev_field(path, "PSAT", &psat) == 0) {
            u8 *ps = plat_mmio(psat, 4);
            if (ps && (RD32(ps) & 3)) {
                WR32(ps, RD32(ps) & ~3UL);
                mdelay(10);
                DBG("sdhci: ACPI %s to D0 (PSAT %08lx)\n", path, psat);
            }
        }
        if (!strncmp(hid, "80860F1", 7)) byt_ocp();
        if (add_slot(phys, NULL, 0, fixed, hid, path, uid, irq, pnpkey)) found++;
    }
    return found;
}

int sdhci_probe_acpi(void)
{
    return probe_acpi(0, 0);
}

/* Windows 95/98/ME: the Configuration Manager gave the registers of an ACPI devnode. The ACPI
   namespace still says what the host is (eMMC, SD, SDIO, removable); a host it does not
   describe is set up from its registers alone. The cards get disk key 5344xxxxh with xxxx
   the low 16 bits of key. 0 when a slot was added. */
int sdhci_probe_mmio(u32 phys, u32 len, int irq, u32 key)
{
    int i;
    if (!phys || len < 0x100) return -1;
    for (i = 0; i < nslot; i++)
        if (slots[i]->phys == phys) {
            /* started at the boot of Windows (core_pnp_start_boot): now the devnode's */
            struct sdslot *s = slots[i];
            if (s->pci.vendor || s->pnpkey) return -1;
            s->pnpkey = key + 1;
            s->bd.key = 0x53440000UL | (key & 0xFFFF);
            return 0;
        }
    if (probe_acpi(phys, key + 1)) return 0;
    return add_slot(phys, NULL, 0, 0, "", "", 0, (u32)irq, key + 1) ? 0 : -1;
}

/* ---- platform handover, Plug and Play ---- */

int sd_count(void)
{
    int i, n = 0;
    for (i = 0; i < nslot; i++)
        if (!slots[i]->fixed) n++;
    return n;
}

int sd_slots(void)
{
    return nslot;
}

int sd_slot_info(int i, struct sdinfo *o)
{
    struct sdslot *s;
    if (i < 0 || i >= nslot) return -1;
    s = slots[i];
    memset(o, 0, sizeof(*o));
    o->bdf = s->pci.vendor ? PCI_BDF(&s->pci) : 0xFFFFFFFFUL;
    o->mmio = s->phys;
    o->irq = s->irq;
    o->uid = s->uid;
    strlcpy(o->hid, s->hid, sizeof(o->hid));
    strlcpy(o->path, s->path, sizeof(o->path));
    o->slot = s->slot;
    o->fixed = s->fixed;
    o->borrow = s->borrow;
    o->mmc = s->mmc;
    o->disk = s->registered ? &s->bd : NULL;
    return 0;
}

/* hand the controllers over (DOS core -> VxD, VxD -> DOS core): our own slots are switched
   off, the BIOS's keep their state */
void sdhci_shutdown(void)
{
    int i;
    for (i = 0; i < nslot; i++) {
        struct sdslot *s = slots[i];
        if (!s->r || s->borrow) continue;
        set_clock(s, 0);
        set_power(s, 0);
    }
}

/* DOS core after Windows: our cards are set up again */
void sdhci_resume(void)
{
    int i;
    for (i = 0; i < nslot; i++) {
        struct sdslot *s = slots[i];
        if (!s->r || s->borrow) continue;
        if (own(s)) continue;
        if (s->registered && card_init(s)) {
            kprintf("sd%d: card lost\n", s->no);
            s->card = 0;
            s->bd.present = 0;
            blk_unregister(&s->bd);
            s->registered = 0;
        }
        s->lastpoll = plat_ms();
    }
}

/* key: bus << 8 | dev << 3 | fn of a PCI function, or the key of sdhci_probe_mmio */
void sdhci_remove(u32 key)
{
    int i;
    for (i = 0; i < nslot; i++) {
        struct sdslot *s = slots[i];
        if (!s->r) continue;
        if (s->pci.vendor ? PCI_BDF(&s->pci) != key : s->pnpkey != key + 1) continue;
        if (s->registered) {
            s->card = 0;
            s->bd.present = 0;
            blk_unregister(&s->bd);
            s->registered = 0;
        }
        if (!s->borrow) {
            set_clock(s, 0);
            set_power(s, 0);
        }
        s->r = NULL;                                /* the slot stays in the table, unused */
        s->phys = 0;
    }
}
