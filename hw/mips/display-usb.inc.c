/*
 * Book2 JZ4750 LCD scanout + native QEMU OHCI wiring.
 * Include IN PLACE OF the old complete LCD section in book2.c.
 * Existing book2_register_types() continues registering book2_lcd_info.
 * No guest CPU registers, program counter, or firmware-specific hooks.
 * See analysis/display-usb/INTEGRATION.md for scope and evidence.
 */
#include "qemu/bswap.h"
#include "hw/usb/usb.h"

#define TYPE_BOOK2_LCD "book2-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(Book2LcdState, BOOK2_LCD)

#define B2LCD_CFG       0x000
#define B2LCD_VAT       0x00c
#define B2LCD_DAH       0x010
#define B2LCD_DAV       0x014
#define B2LCD_CTRL      0x030
#define B2LCD_STATE     0x034
#define B2LCD_DA(n)     (0x040 + (n) * 0x10)
#define B2LCD_SA(n)     (0x044 + (n) * 0x10)
#define B2LCD_FID(n)    (0x048 + (n) * 0x10)
#define B2LCD_CMD(n)    (0x04c + (n) * 0x10)
#define B2LCD_OFFS(n)   (0x060 + (n) * 0x10)
#define B2LCD_PW(n)     (0x064 + (n) * 0x10)
#define B2LCD_CNUM(n)   (0x068 + (n) * 0x10)
#define B2LCD_DSIZE(n)  (0x06c + (n) * 0x10)
#define B2LCD_OSDC      0x100
#define B2LCD_OSDCTRL   0x104
#define B2LCD_OSDS      0x108
#define B2LCD_BGC       0x10c
#define B2LCD_KEY(n)    (0x110 + (n) * 4)
#define B2LCD_ALPHA     0x118
#define B2LCD_ALPHAEN   (1u << 2)  /* OSDC: enable alpha blending */
#define B2LCD_ALPHAMD   (1u << 1)  /* OSDC: per-pixel alpha instead of ALPHA */
#define B2LCD_XYP(n)    (0x120 + (n) * 4)
#define B2LCD_SIZE(n)   (0x128 + (n) * 4)
#define B2LCD_NEWDES    (1u << 28)
#define B2LCD_ENA       (1u << 3)
#define B2LCD_DIS       (1u << 4)
#define B2LCD_BEDN      (1u << 6)
#define B2LCD_PEDN      (1u << 5)
#define B2LCD_RGB555    (1u << 27)
#define B2LCD_QD        (1u << 7)
#define B2LCD_EOF       (1u << 5)
#define B2LCD_SOF       (1u << 4)
#define B2LCD_OFU       (1u << 3)
#define B2LCD_IFU0      (1u << 2)
#define B2LCD_IFU1      (1u << 1)
#define B2LCD_LDD       (1u << 0)
#define B2LCD_FLAGS     0xbfu
#define B2LCD_EOFINT    (1u << 30)
#define B2LCD_SOFINT    (1u << 31)
#define B2LCD_PAL       (1u << 28)
#define B2LCD_SLCD_CMD  (1u << 29)
#define B2LCD_LEN       0x00ffffffu
#define B2LCD_MAX_DIM   2048
#define B2LCD_PERIOD_NS 16393443LL /* Book2 panel target 61 Hz; CPM clock not wired */
#define B2LCD_R(s, a)   ((s)->regs[(a) / 4])

typedef struct Book2LcdDma {
    uint32_t addr, cmd, offsize, pagewidth, size;
    bool valid;
} Book2LcdDma;

struct Book2LcdState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t regs[0x200 / 4];
    Book2LcdDma dma[2];
    uint16_t palette[256];
    uint32_t irq_frame_flags;
    QemuConsole *con;
    uint8_t *line;
    size_t line_capacity;
    /* Snapshot of the last composed frame, used to skip host redraws when
     * the guest did not change anything (a static desktop repaints 61x/s). */
    uint32_t *frame_snapshot;
    size_t frame_snapshot_size;
    bool frame_snapshot_valid;
};

static bool book2_lcd_running(Book2LcdState *s)
{
    return (B2LCD_R(s, B2LCD_CTRL) & B2LCD_ENA) != 0;
}

static void book2_lcd_update_irq(Book2LcdState *s)
{
    uint32_t st = B2LCD_R(s, B2LCD_STATE);
    uint32_t ctrl = B2LCD_R(s, B2LCD_CTRL);
    uint32_t eligible = st & ~(B2LCD_EOF | B2LCD_SOF);
    uint32_t osd = B2LCD_R(s, B2LCD_OSDS);
    uint32_t osdc = B2LCD_R(s, B2LCD_OSDC);
    uint32_t enabled;

    /* Despite the M suffix, JZ4750 ops.h enables interrupts by SETTING M. */
    eligible |= st & s->irq_frame_flags;
    enabled = ((ctrl >> 8) & 0x3f) | (ctrl & B2LCD_QD);
    qemu_set_irq(s->irq, (eligible & enabled) ||
                 (osd & osdc & ((3u << 10) | (3u << 14))));
}

static bool book2_lcd_dma_read(uint32_t addr, void *buf, size_t len)
{
    /* JZ4750 has a 29-bit physical bus. DMA is independent of CPU segments. */
    hwaddr pa = addr & 0x1fffffff;
    if (len > 0x20000000ULL - pa) {
        return false;
    }
    return address_space_read(&address_space_memory, pa,
                              MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static bool book2_lcd_channel_on(Book2LcdState *s, unsigned n)
{
    uint32_t osdc = B2LCD_R(s, B2LCD_OSDC);
    return (osdc & 1) ? (osdc & (1u << (3 + n))) != 0 : n == 0;
}

static bool book2_lcd_fetch(Book2LcdState *s, unsigned n)
{
    uint8_t raw[32];
    uint32_t words[8];
    Book2LcdDma *d = &s->dma[n];
    unsigned count, i;
    bool extended = B2LCD_R(s, B2LCD_CFG) & B2LCD_NEWDES;

    d->valid = false;
    /* A bounded walk handles a palette/SLCD command preceding a frame,
     * while preventing a cyclic non-frame list from hanging the host. */
    for (count = 0; count < 16; count++) {
        uint32_t da = B2LCD_R(s, B2LCD_DA(n)) & ~3u;
        uint32_t cmd;
        if (!da || !book2_lcd_dma_read(da, raw, extended ? 32 : 16)) {
            break;
        }
        memset(words, 0, sizeof(words));
        for (i = 0; i < (extended ? 8 : 4); i++) {
            words[i] = ldl_le_p(raw + i * 4);
        }
        cmd = words[3];
        B2LCD_R(s, B2LCD_DA(n)) = words[0];
        B2LCD_R(s, B2LCD_SA(n)) = words[1];
        B2LCD_R(s, B2LCD_FID(n)) = words[2];
        B2LCD_R(s, B2LCD_CMD(n)) = cmd;
        B2LCD_R(s, B2LCD_OFFS(n)) = words[4];
        B2LCD_R(s, B2LCD_PW(n)) = words[5];
        B2LCD_R(s, B2LCD_CNUM(n)) = words[6];
        B2LCD_R(s, B2LCD_DSIZE(n)) = words[7];
        if (cmd & B2LCD_PAL) {
            uint8_t pal[512];
            size_t len = MIN((size_t)(cmd & B2LCD_LEN) * 4, sizeof(pal));
            if (!book2_lcd_dma_read(words[1], pal, len)) {
                break;
            }
            for (i = 0; i < len / 2; i++) {
                s->palette[i] = lduw_le_p(pal + i * 2);
            }
            continue;
        }
        if (cmd & B2LCD_SLCD_CMD) {
            continue; /* No external smart-LCD panel is wired on Book2. */
        }
        d->addr = words[1];
        d->cmd = cmd;
        d->offsize = words[4];
        d->pagewidth = words[5];
        d->size = words[7];
        d->valid = (cmd & B2LCD_LEN) != 0;
        if (d->valid) {
            return true;
        }
        break;
    }
    B2LCD_R(s, B2LCD_STATE) |= n ? B2LCD_IFU1 : B2LCD_IFU0;
    return false;
}

static unsigned book2_lcd_expand5(unsigned v)
{
    return (v << 3) | (v >> 2);
}

static uint32_t book2_lcd_color16(uint16_t p, bool rgb555)
{
    unsigned r = book2_lcd_expand5((p >> (rgb555 ? 10 : 11)) & 31);
    unsigned g = rgb555 ? book2_lcd_expand5((p >> 5) & 31) :
                         (((p >> 5) & 63) << 2) | ((p >> 9) & 3);
    unsigned b = book2_lcd_expand5(p & 31);
    return (r << 16) | (g << 8) | b;
}

/* 64K-entry RGB565->XRGB8888 lookup.  The Book2 desktop is 800x480 RGB565
 * repainted at 61 Hz, so per-pixel bit shuffling dominated every host
 * redraw.  A table turns that into a single load. */
static uint32_t *book2_lcd_rgb565_lut;

static const uint32_t *book2_lcd_get_rgb565_lut(void)
{
    if (!book2_lcd_rgb565_lut) {
        uint32_t *lut = g_new(uint32_t, 65536);

        for (unsigned i = 0; i < 65536; i++) {
            lut[i] = book2_lcd_color16((uint16_t)i, false);
        }
        book2_lcd_rgb565_lut = lut;
    }
    return book2_lcd_rgb565_lut;
}

static uint32_t book2_lcd_pixel(Book2LcdState *s, const uint8_t *p,
                               unsigned x, unsigned fmt, bool rgb555)
{
    uint32_t ctrl = B2LCD_R(s, B2LCD_CTRL);
    unsigned index, bit, bits;
    switch (fmt) {
    case 4:
        return book2_lcd_color16(lduw_le_p(p + x * 2), rgb555);
    case 5: /* LCD BPP_18_24 is 32-bit memory storage, not packed 24-bit. */
        return ldl_le_p(p + x * 4) & 0xffffff;
    default:
        bits = 1u << fmt;
        bit = x * bits;
        index = (p[bit / 8] >> ((ctrl & B2LCD_PEDN) ? (bit & 7) :
                                        8 - bits - (bit & 7))) & ((1u << bits) - 1);
        return book2_lcd_color16(s->palette[index], false);
    }
}

/*
 * Foreground 0 is the 18/24-bit OSD layer; its stored word carries an 8-bit
 * per-pixel alpha in the high byte (the firmware's cursor uses 0 for fully
 * transparent and 0xff for solid).  With OSDC.ALPHAMD set the alpha comes
 * from the pixel itself; otherwise the global ALPHA register applies.
 */
static uint32_t book2_lcd_pixel_alpha(Book2LcdState *s, const uint8_t *p,
                                      unsigned x, unsigned fmt, bool rgb555,
                                      uint32_t osdc)
{
    uint32_t word;

    if (fmt != 5) {
        return 255;
    }
    word = ldl_le_p(p + x * 4);
    if (!(osdc & B2LCD_ALPHAMD)) {
        return 255; /* Global ALPHA path is applied by the caller. */
    }
    return word >> 24;
}


static bool book2_lcd_geometry(Book2LcdState *s, unsigned *w, unsigned *h)
{
    uint32_t dah = B2LCD_R(s, B2LCD_DAH);
    uint32_t dav = B2LCD_R(s, B2LCD_DAV);
    unsigned xs = dah >> 16, xe = dah & 0xffff;
    unsigned ys = dav >> 16, ye = dav & 0xffff;
    if (xe <= xs || ye <= ys || xe - xs > B2LCD_MAX_DIM ||
        ye - ys > B2LCD_MAX_DIM) {
        return false;
    }
    *w = xe - xs;
    *h = ye - ys;
    return true;
}

static void book2_lcd_draw_channel(Book2LcdState *s, unsigned n,
                                  DisplaySurface *surf, unsigned pw, unsigned ph)
{
    Book2LcdDma *d = &s->dma[n];
    uint32_t osdc = B2LCD_R(s, B2LCD_OSDC);
    uint32_t ctrl = B2LCD_R(s, B2LCD_CTRL);
    uint32_t fmtctrl = n ? B2LCD_R(s, B2LCD_OSDCTRL) : ctrl;
    unsigned fmt = fmtctrl & 7;
    bool rgb555 = fmtctrl & (n ? (1u << 4) : B2LCD_RGB555);
    uint32_t ds = d->size ? d->size : B2LCD_R(s, B2LCD_SIZE(n));
    uint32_t xy = (osdc & 1) ? B2LCD_R(s, B2LCD_XYP(n)) : 0;
    unsigned dx = xy & 0xffff, dy = xy >> 16;
    unsigned w = (osdc & 1) && (ds & 0xffff) ? ds & 0xffff : pw;
    unsigned h = (osdc & 1) && (ds >> 16) ? ds >> 16 : ph;
    size_t rowbytes, stride, remaining;
    uint32_t key = B2LCD_R(s, B2LCD_KEY(n));
    bool blend = (osdc & B2LCD_ALPHAEN) != 0;
    unsigned alpha = blend ? (B2LCD_R(s, B2LCD_ALPHA) & 255) : 255;
    unsigned bpp, y, x;

    if (!d->valid || !book2_lcd_channel_on(s, n) || fmt > 5 ||
        w > B2LCD_MAX_DIM || h > B2LCD_MAX_DIM || dx >= pw || dy >= ph) {
        return;
    }
    bpp = fmt == 5 ? 32 : (1u << fmt);
    rowbytes = DIV_ROUND_UP((size_t)w * bpp, 32) * 4;
    if (d->pagewidth) {
        if ((uint64_t)d->pagewidth * 4 < rowbytes) {
            return;
        }
        rowbytes = (size_t)d->pagewidth * 4;
    }
    /* Reject impossible row sizes before allocating host storage. */
    if (rowbytes > B2LCD_MAX_DIM * 4 || d->offsize > 0x1fffffff / 4) {
        return;
    }
    stride = rowbytes + (size_t)d->offsize * 4;
    remaining = (size_t)(d->cmd & B2LCD_LEN) * 4;
    if (s->line_capacity < rowbytes) {
        s->line = g_realloc(s->line, rowbytes);
        s->line_capacity = rowbytes;
    }
    for (y = 0; y < MIN(h, ph - dy); y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surf) +
                                   (dy + y) * surface_stride(surf));
        uint64_t addr = (uint64_t)(d->addr & 0x1fffffff) + y * (uint64_t)stride;
        if (remaining < rowbytes || addr + rowbytes > 0x20000000ULL ||
            !book2_lcd_dma_read(addr, s->line, rowbytes)) {
            break;
        }
        remaining -= rowbytes;
        if (ctrl & B2LCD_BEDN) {
            for (x = 0; x < rowbytes; x += 4) {
                uint32_t v = ldl_le_p(s->line + x);
                stl_le_p(s->line + x, bswap32(v));
            }
        }
        for (x = 0; x < MIN(w, pw - dx); x++) {
            uint32_t color;
            unsigned pix_alpha = alpha;

            if (fmt == 4 && !rgb555 && !blend) {
                /* Opaque 16bpp layer: table lookup, no per-pixel work. */
                dst[dx + x] = book2_lcd_get_rgb565_lut()[
                    lduw_le_p(s->line + x * 2)];
                continue;
            }
            color = book2_lcd_pixel(s, s->line, x, fmt, rgb555);

            if ((key & (1u << 31)) &&
                (((color == (key & 0xffffff)) != 0) !=
                 ((key & (1u << 30)) != 0))) {
                continue;
            }
            if (blend) {
                /*
                 * ALPHAMD selects a per-pixel alpha layer.  The Firmware's
                 * cursor supplies 0/0xff per pixel; a pixel with alpha 0 is
                 * fully transparent and must leave the layer below intact.
                 */
                if (osdc & B2LCD_ALPHAMD) {
                    pix_alpha = book2_lcd_pixel_alpha(s, s->line, x, fmt,
                                                      rgb555, osdc);
                    if (!pix_alpha) {
                        continue;
                    }
                }
            }
            if (pix_alpha != 255) {
                uint32_t old = dst[dx + x];
                unsigned r = (((color >> 16) & 255) * pix_alpha +
                              ((old >> 16) & 255) * (255 - pix_alpha) + 127) / 255;
                unsigned g = (((color >> 8) & 255) * pix_alpha +
                              ((old >> 8) & 255) * (255 - pix_alpha) + 127) / 255;
                unsigned b = ((color & 255) * pix_alpha +
                              (old & 255) * (255 - pix_alpha) + 127) / 255;
                color = (r << 16) | (g << 8) | b;
            }
            /* Native-endian PIXMAN_x8r8g8b8 word. On LE host: B,G,R,X. */
            dst[dx + x] = color;
        }
    }
}

static bool book2_lcd_gfx_update(void *opaque)
{
    Book2LcdState *s = opaque;
    DisplaySurface *surf;
    unsigned w, h, y;
    uint32_t bg = B2LCD_R(s, B2LCD_BGC) & 0xffffff;
    size_t bytes;
    bool changed = false;

    if (!book2_lcd_running(s) || !book2_lcd_geometry(s, &w, &h)) {
        return true;
    }
    qemu_console_resize(s->con, w, h);
    surf = qemu_console_surface(s->con);
    if (!surf) {
        return true;
    }
    /* Only the background needs clearing; the foreground channels paint
     * over it.  A contiguous surface row makes this a per-row memset. */
    for (y = 0; y < h; y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surf) + y * surface_stride(surf));
        for (unsigned x = 0; x < w; x++) {
            dst[x] = bg;
        }
    }
    /* Z-order follows the hardware: FG1 is the full-screen primary plane,
     * FG0 is the overlay that carries the mouse cursor.  Painting FG0 first
     * would let the opaque FG1 frame erase it. */
    book2_lcd_draw_channel(s, 1, surf, w, h);
    book2_lcd_draw_channel(s, 0, surf, w, h);

    /*
     * The guest composes a complete frame every refresh period.  Pushing an
     * unchanged frame to the UI forces a full host texture upload for
     * nothing, which is what makes an otherwise static desktop expensive.
     * Compare against the previous composition and only publish differences.
     */
    bytes = (size_t)w * h * sizeof(uint32_t);
    if (s->frame_snapshot_size != bytes) {
        s->frame_snapshot = g_realloc(s->frame_snapshot, bytes);
        s->frame_snapshot_size = bytes;
        s->frame_snapshot_valid = false;
    }
    for (y = 0; y < h; y++) {
        const void *src = surface_data(surf) + y * surface_stride(surf);
        void *old = (uint8_t *)s->frame_snapshot + (size_t)y * w * sizeof(uint32_t);

        if (!s->frame_snapshot_valid || memcmp(src, old, w * sizeof(uint32_t))) {
            memcpy(old, src, w * sizeof(uint32_t));
            changed = true;
        }
    }
    if (!changed) {
        return true;
    }
    s->frame_snapshot_valid = true;
    qemu_console_update_full(s->con);
    /* UI refresh / QMP screendump must not advance DMA or raise IRQs. */
    return true;
}

static void book2_lcd_invalidate(void *opaque)
{
    /* gfx_update always repaints, including a synchronous QMP screendump. */
}

static void book2_lcd_frame_tick(void *opaque)
{
    Book2LcdState *s = opaque;
    unsigned n;
    if (!book2_lcd_running(s)) {
        return;
    }
    for (n = 0; n < 2; n++) {
        uint32_t cmd;
        if (!book2_lcd_channel_on(s, n) || !book2_lcd_fetch(s, n)) {
            continue;
        }
        cmd = s->dma[n].cmd;
        B2LCD_R(s, B2LCD_STATE) |= B2LCD_SOF | B2LCD_EOF;
        if (cmd & B2LCD_SOFINT) {
            s->irq_frame_flags |= B2LCD_SOF;
        }
        if (cmd & B2LCD_EOFINT) {
            s->irq_frame_flags |= B2LCD_EOF;
        }
        B2LCD_R(s, B2LCD_OSDS) |= n ? (3u << 14) : (3u << 10);
    }
    book2_lcd_update_irq(s);
    /*
     * The frame timer only advances the emulated display engine (DMA
     * descriptors, SOF/EOF status and IRQs).  Repainting is left to the
     * display refresh callback and to QMP screendumps: forcing a second
     * full 800x480 decode here at 61 Hz would double the host work while
     * producing the identical image.
     */
    if (B2LCD_R(s, B2LCD_CTRL) & B2LCD_DIS) {
        B2LCD_R(s, B2LCD_CTRL) &= ~B2LCD_ENA;
        B2LCD_R(s, B2LCD_STATE) |= B2LCD_LDD;
        book2_lcd_update_irq(s);
    } else {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + B2LCD_PERIOD_NS);
    }
}

static const GraphicHwOps book2_lcd_graphic_ops = {
    .gfx_update = book2_lcd_gfx_update,
    .invalidate = book2_lcd_invalidate,
};

static uint64_t book2_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2LcdState *s = opaque;
    uint32_t v;
    if (addr >= sizeof(s->regs) || (addr & 3) + size > 4) {
        return 0;
    }
    v = B2LCD_R(s, addr & ~3);
    return (v >> ((addr & 3) * 8)) & (UINT32_MAX >> ((4 - size) * 8));
}

static void book2_lcd_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Book2LcdState *s = opaque;
    unsigned off = addr & ~3u, shift = (addr & 3) * 8;
    uint32_t mask, old, val;
    if (addr >= sizeof(s->regs) || (addr & 3) + size > 4) {
        return;
    }
    mask = (UINT32_MAX >> ((4 - size) * 8)) << shift;
    old = B2LCD_R(s, off);
    val = (old & ~mask) | (((uint32_t)value << shift) & mask);
    switch (off) {
    case B2LCD_STATE:
        /* W0C, NOT W1C. Preserve lanes not addressed by this access. */
        B2LCD_R(s, off) = old & (val | ~mask) & B2LCD_FLAGS;
        s->irq_frame_flags &= B2LCD_R(s, off);
        break;
    case B2LCD_OSDS:
        B2LCD_R(s, off) = (old & (val | ~mask)) | 1; /* READY */
        break;
    case B2LCD_OSDCTRL:
        B2LCD_R(s, off) = val & ~(1u << 3); /* size-change handshake completes */
        B2LCD_R(s, B2LCD_OSDS) |= 1;
        break;
    case B2LCD_CTRL:
        B2LCD_R(s, off) = val;
        if ((old & B2LCD_ENA) && !(val & B2LCD_ENA)) {
            timer_del(s->timer);
            B2LCD_R(s, B2LCD_STATE) |= B2LCD_QD;
        } else if (!(old & B2LCD_ENA) && (val & B2LCD_ENA)) {
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        /* Disable when already idle must still complete without a frame. */
        if ((val & B2LCD_DIS) && !(val & B2LCD_ENA)) {
            B2LCD_R(s, B2LCD_STATE) |= B2LCD_LDD;
        }
        break;
    default:
        B2LCD_R(s, off) = val;
        break;
    }
    book2_lcd_update_irq(s);
}

static const MemoryRegionOps book2_lcd_ops = {
    .read = book2_lcd_read,
    .write = book2_lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void book2_lcd_reset(DeviceState *dev)
{
    Book2LcdState *s = BOOK2_LCD(dev);
    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->dma, 0, sizeof(s->dma));
    memset(s->palette, 0, sizeof(s->palette));
    s->irq_frame_flags = 0;
    s->frame_snapshot_valid = false;
    B2LCD_R(s, B2LCD_OSDS) = 1;
    qemu_set_irq(s->irq, 0);
}

static void book2_lcd_realize(DeviceState *dev, Error **errp)
{
    Book2LcdState *s = BOOK2_LCD(dev);
    s->con = qemu_graphic_console_create(dev, 0, &book2_lcd_graphic_ops, s);
    qemu_console_resize(s->con, 800, 480); /* physical Book2 panel, until timings */
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_lcd_frame_tick, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_lcd_ops, s,
                          "book2-lcd", sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    B2LCD_R(s, B2LCD_OSDS) = 1;
}

static void book2_lcd_finalize(Object *obj)
{
    Book2LcdState *s = BOOK2_LCD(obj);
    if (s->timer) {
        timer_free(s->timer);
    }
    g_free(s->line);
    g_free(s->frame_snapshot);
}

static void book2_lcd_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_lcd_realize;
    device_class_set_legacy_reset(dc, book2_lcd_reset);
    dc->user_creatable = false;
}

static const TypeInfo book2_lcd_info = {
    .name = TYPE_BOOK2_LCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2LcdState),
    .instance_finalize = book2_lcd_finalize,
    .class_init = book2_lcd_class_init,
};

/* Call from book2_init(), after INTC exists. Select USB_OHCI_SYSBUS + USB_HID.
 * This is the existing QEMU little-endian OHCI model, not an OHCI reimplementation.
 * Pass attach_mouse=false to manage USB devices entirely with -device instead. */
static void book2_init_ohci(DeviceState *intc, bool attach_mouse)
{
    DeviceState *ohci = qdev_new("sysbus-ohci");
    qdev_prop_set_uint32(ohci, "num-ports", 2);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ohci), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ohci), 0, JZ_UHC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ohci), 0,
                       qdev_get_gpio_in(intc, JZ_IRQ_UHC));
    if (attach_mouse) {
        BusState *bus = qdev_get_child_bus(ohci, "usb-bus.0");
        if (!bus) {
            error_report("book2: native OHCI USB bus was not created");
            exit(1);
        }
        usb_create_simple(USB_BUS(bus), "usb-mouse");
    }
}
