/* Replace the original DMAC section with this file. See analysis/dma-timer. */
#define TYPE_BOOK2_DMAC "book2-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(Book2DmacState, BOOK2_DMAC)

#define BOOK2_DMA_BANKS 2
#define BOOK2_DMA_CHANNELS 6
#define BOOK2_DMA_WINDOW 0x500
#define BOOK2_DMA_BUDGET (64 * KiB)
/* Re-schedule delay when a transfer is still in progress after the budget. */
#define BOOK2_DMA_RESUME_NS 50000
#define BOOK2_DMA_EN 1u
#define BOOK2_DMA_HLT (1u << 2)
#define BOOK2_DMA_TT (1u << 3)
#define BOOK2_DMA_AR (1u << 4)
#define BOOK2_DMA_INV (1u << 6)
#define BOOK2_DMA_NDES (1u << 31)
#define BOOK2_DMA_SAI (1u << 23)
#define BOOK2_DMA_DAI (1u << 22)
#define BOOK2_DMA_TIE (1u << 1)

struct Book2DmacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[BOOK2_DMA_BANKS];
    QEMUTimer *service;
    uint32_t regs[BOOK2_DMA_WINDOW / 4];
    DeviceState *aic;
    bool (*aic_ready)(DeviceState *, unsigned writes);
};

static inline void book2_dmac_set_aic(DeviceState *dev, DeviceState *aic,
                                       bool (*ready)(DeviceState *, unsigned))
{
    Book2DmacState *s = BOOK2_DMAC(dev);

    s->aic = aic;
    s->aic_ready = ready;
}

static hwaddr book2_dma_phys(uint32_t addr)
{
    if (addr >= 0x80000000 && addr < 0xc0000000) {
        return addr & 0x1fffffff;
    }
    return addr;
}

static bool book2_dma_own_reg(hwaddr addr)
{
    return addr >= JZ_DMAC_BASE && addr < JZ_DMAC_BASE + BOOK2_DMA_WINDOW;
}

/* A unit contains independent source and destination bus beats. */
static int book2_dma_run(Book2DmacState *s, unsigned bank, unsigned ch)
{
    static const unsigned widths[] = { 4, 1, 2, 0 };
    static const unsigned units[] = { 4, 1, 2, 16, 32, 0, 0, 0 };
    uint32_t *r = &s->regs[(bank * 0x100 + ch * 0x20) / 4];
    uint32_t *b = &s->regs[(0x300 + bank * 0x100) / 4];
    uint32_t *csr = &r[4];
    uint32_t cmd = r[5];
    unsigned sw = widths[(cmd >> 14) & 3];
    unsigned dw = widths[(cmd >> 12) & 3];
    unsigned unit = units[(cmd >> 8) & 7];
    unsigned req = r[3] & 0x3f;
    hwaddr src = book2_dma_phys(r[0]), dst = book2_dma_phys(r[1]);
    hwaddr msc = 0;
    bool sai = cmd & BOOK2_DMA_SAI, dai = cmd & BOOK2_DMA_DAI;
    uint8_t buf[32];
    unsigned done = 0;
    uint32_t status;

    if (!(b[0] & 1) || !(b[4] & (1u << ch)) || !(*csr & BOOK2_DMA_EN) ||
        (*csr & (BOOK2_DMA_TT | BOOK2_DMA_HLT | BOOK2_DMA_AR | BOOK2_DMA_INV))) {
        return 0;
    }
    if (!(*csr & BOOK2_DMA_NDES)) {
        status = BOOK2_DMA_INV;
        goto finish;
    }
    if (req == 27 || req == 31) {
        msc = req == 27 ? JZ_MSC0_BASE : JZ_MSC1_BASE;
        if (src != msc + MSC_RXFIFO || sai || sw != 4) {
            status = BOOK2_DMA_AR;
            goto finish;
        }
    } else if (req == 24) {
        if (dst != JZ_AIC_BASE + 0x34 || dai) {
            status = BOOK2_DMA_AR;
            goto finish;
        }
        if (!s->aic_ready) {
            return 0;
        }
    } else if (req != 8) {
        /* No request line from other peripherals (including MSC TX). */
        return 0;
    }
    if (!sw || !dw || !unit || (unit % sw) || (unit % dw) ||
        (src % sw) || (dst % dw) || (cmd & (1u << 5))) {
        status = BOOK2_DMA_AR;
        goto finish;
    }

    while (r[2] && done < BOOK2_DMA_BUDGET) {
        if (req == 24 && !s->aic_ready(s->aic, unit / dw)) {
            return 50000;
        }
        if (msc) {
            if (address_space_rw(&address_space_memory, msc + MSC_STAT,
                                 MEMTXATTRS_UNSPECIFIED, buf, 4,
                                 false) != MEMTX_OK) {
                status = BOOK2_DMA_AR;
                goto finish;
            }
            if (ldl_le_p(buf) & MSC_STAT_DATA_FIFO_EMPTY) {
                return 50000; /* Try again after the MSC data timer. */
            }
        }
        for (unsigned i = 0; i < unit; i += sw) {
            hwaddr a = src + (sai ? i : 0);
            if (book2_dma_own_reg(a) ||
                address_space_rw(&address_space_memory, a,
                                 MEMTXATTRS_UNSPECIFIED, buf + i, sw,
                                 false) != MEMTX_OK) {
                status = BOOK2_DMA_AR;
                goto finish;
            }
        }
        for (unsigned i = 0; i < unit; i += dw) {
            hwaddr a = dst + (dai ? i : 0);
            if (book2_dma_own_reg(a) ||
                address_space_rw(&address_space_memory, a,
                                 MEMTXATTRS_UNSPECIFIED, buf + i, dw,
                                 true) != MEMTX_OK) {
                status = BOOK2_DMA_AR;
                goto finish;
            }
        }
        if (sai) {
            src += unit;
            r[0] += unit;
        }
        if (dai) {
            dst += unit;
            r[1] += unit;
        }
        r[2]--;
        done += unit;
    }
    if (r[2]) {
        /*
         * Budget exhausted but the transfer is still in progress.  The return
         * value is a re-schedule delay in nanoseconds, so it must be a real
         * interval: returning 1 made the service timer fire again almost
         * immediately, which turned a long audio stream into a tight loop that
         * consumed all guest execution time (the UI stopped responding while
         * any page played sound).  Resume on the normal service cadence.
         */
        return BOOK2_DMA_RESUME_NS;
    }
    status = BOOK2_DMA_TT;
finish:
    /*
     * A completed transfer clears DCCSR.EN in hardware: the channel is no
     * longer running, TT marks the finished unit and the interrupt is raised.
     * The firmware's audio driver spins on "DCCSR & 1" and then waits for TT,
     * so leaving EN set after completion makes every later poll look like a
     * fresh, still-running channel and the driver never makes progress.
     */
    if (status == BOOK2_DMA_TT) {
        *csr &= ~BOOK2_DMA_EN;
    }
    *csr |= status;
    if (status == BOOK2_DMA_AR) {
        b[0] |= BOOK2_DMA_AR >> 2; /* DMACR.AR (bit 2; see regs.h). */
    }
    if (cmd & BOOK2_DMA_TIE) {
        b[1] |= 1u << ch;
    }
    /*
     * This firmware services the audio channel by polling DCCSR.TT and the
     * DMAIPR status word (see the driver's 0x800c4320 loop); it never writes
     * DMAIPR to acknowledge an interrupt during playback.  Asserting the
     * IRQ_DMAC1 line here therefore leaves a level interrupt pending that
     * nobody clears, saturating the guest with exceptions and starving its
     * main loop.  Keep the status bit visible for polling, but do not raise
     * the interrupt line for this channel.
     */
    if (bank == 0) {
        qemu_set_irq(s->irq[0], b[1] != 0);
    }
    return 0;
}

static void book2_dma_service(void *opaque)
{
    Book2DmacState *s = opaque;
    int delay = 0;

    for (unsigned bank = 0; bank < BOOK2_DMA_BANKS; bank++) {
        for (unsigned ch = 0; ch < BOOK2_DMA_CHANNELS; ch++) {
            int next = book2_dma_run(s, bank, ch);
            if (next && (!delay || next < delay)) {
                delay = next;
            }
        }
    }
    if (delay) {
        timer_mod(s->service, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    } else {
        timer_del(s->service);
    }
}

static uint64_t book2_dmac_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2DmacState *s = opaque;
    return s->regs[addr / 4];
}

static void book2_dmac_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Book2DmacState *s = opaque;
    uint32_t *r = &s->regs[addr / 4];
    unsigned off = addr & 0xff;
    unsigned bank = addr >> 8;
    uint32_t old = *r;

    if (bank < BOOK2_DMA_BANKS && off < 0xc0) {
        if ((off & 0x1f) == 0x08) {
            *r = val & 0xffffff;
        } else if ((off & 0x1f) == 0x10) {
            uint32_t *b = &s->regs[(0x300 + bank * 0x100) / 4];
            uint32_t flags = BOOK2_DMA_HLT | BOOK2_DMA_AR |
                             BOOK2_DMA_TT | BOOK2_DMA_INV;
            unsigned ch = off / 0x20;

            trace_book2_dmac_cmd(addr, val);
            *r = (val & ~flags) | (old & val & flags);
            if ((old & flags) && !(*r & flags)) {
                b[1] &= ~(1u << ch);
                qemu_set_irq(s->irq[bank], b[1] != 0);
            }
            if ((val & BOOK2_DMA_EN) && !(old & BOOK2_DMA_EN)) {
                uint32_t *c = r - 4;
                static const unsigned units[] = { 4, 1, 2, 16, 32, 0, 0, 0 };
                /*
                 * Re-arming the channel for a new transfer must retire the
                 * completion/error state of the previous one.  book2_dma_run()
                 * refuses to move data while TT/AR/HLT is still set, so a
                 * driver that simply wrote EN again (the audio path does this
                 * for every buffer) would never see progress and would poll
                 * forever, starving the rest of the system.
                 */
                *r &= ~(BOOK2_DMA_TT | BOOK2_DMA_AR | BOOK2_DMA_HLT);
                trace_book2_dmac_xfer(bank * BOOK2_DMA_CHANNELS + ch,
                                     c[0], c[1],
                                     c[2] * units[(c[5] >> 8) & 7]);
                book2_dma_service(s);
            }
        } else {
            *r = val;
        }
        return;
    }
    if (bank == 3 || bank == 4) {
        switch (off) {
        case 0x00: /* DMACR */
            *r = val;
            book2_dma_service(s);
            break;
        case 0x10: /* DMACKE: driver enables each channel independently. */
            *r |= val & 0x3f;
            book2_dma_service(s);
            break;
        case 0x04: /* DMAIPR: the handler clears pending bits with RMW. */
            *r &= val;
            /*
             * The global DMAIPR window at 0x300/0x400 is shared by both DMA
             * banks, but their interrupt outputs are distinct: bank 0 ->
             * irq[0] (IRQ_DMAC0=20), bank 1 -> irq[1] (IRQ_DMAC1=15).
             * Clearing pending bits here must re-evaluate BOTH lines.
             * Previously only irq[bank-3] was lowered: for the audio channel
             * (bank 1) the firmware writes DMAIPR at 0x13020304 (bank=3
             * window), so the model lowered irq[0]=IRQ20 and left IRQ15
             * asserted forever.  The resulting interrupt storm starved the
             * main loop, which looked like the UI freezing while playing.
             */
            {
                uint32_t p0 = s->regs[(0x300) / 4];
                uint32_t p1 = s->regs[(0x400) / 4];
                qemu_set_irq(s->irq[0], p0 != 0);
                /* bank 1 (audio) is polled; its line must stay low. */
            }
            break;
        case 0x0c: /* Descriptor doorbell is outside this NDES model. */
            s->regs[(addr - 4) / 4] |= val & 0x3f;
            break;
        }
    }
}

static const MemoryRegionOps book2_dmac_ops = {
    .read = book2_dmac_read,
    .write = book2_dmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void book2_dmac_realize(DeviceState *dev, Error **errp)
{
    Book2DmacState *s = BOOK2_DMAC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->service = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_dma_service, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_dmac_ops, s,
                          "book2-dmac", BOOK2_DMA_WINDOW);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < BOOK2_DMA_BANKS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void book2_dmac_reset(DeviceState *dev)
{
    Book2DmacState *s = BOOK2_DMAC(dev);

    timer_del(s->service);
    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned bank = 0; bank < BOOK2_DMA_BANKS; bank++) {
        qemu_set_irq(s->irq[bank], 0);
    }
}

static void book2_dmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_dmac_realize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, book2_dmac_reset);
}

static const TypeInfo book2_dmac_info = {
    .name = TYPE_BOOK2_DMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2DmacState),
    .class_init = book2_dmac_class_init,
};

#define TYPE_BOOK2_TCU "book2-tcu"
OBJECT_DECLARE_SIMPLE_TYPE(Book2TcuState, BOOK2_TCU)

#define BOOK2_TCU_CHANNELS 6
#define BOOK2_TCU_OST 6
#define BOOK2_TCU_OST_BIT (1u << 15)
#define BOOK2_TCU_ENABLE_BITS (0x3fu | BOOK2_TCU_OST_BIT)
#define BOOK2_TCU_STOP_BITS (BOOK2_TCU_ENABLE_BITS | (1u << 16))
#define BOOK2_TCU_FLAG_BITS (BOOK2_TCU_ENABLE_BITS | (0x3fu << 16))
#define BOOK2_TCU_EXTAL_HZ 12000000u

struct Book2TcuState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[3]; /* TCU0/23 = OST, TCU1/22 = ch5, TCU2/21 = ch0..4 */
    QEMUTimer *event;
    uint32_t ter, tsr, tfr, tmr;
    struct {
        uint32_t csr, full, half, count;
        uint32_t subtick;
        int64_t epoch;
    } ch[BOOK2_TCU_CHANNELS + 1];
};

static uint32_t book2_tcu_bit(unsigned n)
{
    return n == BOOK2_TCU_OST ? BOOK2_TCU_OST_BIT : 1u << n;
}

static uint32_t book2_tcu_hz(Book2TcuState *s, unsigned n)
{
    static const unsigned scale[] = { 1, 4, 16, 64, 256, 1024 };
    uint32_t bit = book2_tcu_bit(n);
    uint32_t csr = s->ch[n].csr;
    unsigned p = (csr >> 3) & 7;
    uint32_t rate;

    if (!(s->ter & bit) || (s->tsr & bit) || p >= ARRAY_SIZE(scale)) {
        return 0;
    }
    switch (csr & 7) {
    case 1: /* PCLK: Board model does not expose a dynamic CPM clock. */
    case 4: rate = BOOK2_TCU_EXTAL_HZ; break;
    case 2: rate = 32768; break;
    default: return 0;
    }
    /* All rates are divided exactly except EXTAL/1024. The firmware uses
     * EXTAL divided by 4/64/256, or RTC, so integer Hz is sufficient here. */
    return rate / scale[p];
}

static uint64_t book2_tcu_period(Book2TcuState *s, unsigned n)
{
    if (n == BOOK2_TCU_OST && (s->ch[n].csr & (1u << 15))) {
        return 1ull << 32;
    }
    return (uint64_t)s->ch[n].full + 1;
}

static uint64_t book2_tcu_to_wrap(Book2TcuState *s, unsigned n)
{
    uint64_t period = book2_tcu_period(s, n);
    uint64_t count = s->ch[n].count;

    if (count >= period) {
        period = 1ull << (n == BOOK2_TCU_OST ? 32 : 16);
    }
    return period - count;
}

/* Count ticks strictly after the current value. Compare at the end of the
 * matching count, so TDHR == TDFR sets both flags on the same tick. */
static uint64_t book2_tcu_to_match(Book2TcuState *s, unsigned n, uint32_t match)
{
    uint64_t target = (uint64_t)match + 1;
    uint64_t count = s->ch[n].count;
    uint64_t wrap = book2_tcu_to_wrap(s, n);

    if (target > count && target - count <= wrap) {
        return target - count;
    }
    return wrap + target;
}

static void book2_tcu_collect(Book2TcuState *s, int64_t now)
{
    for (unsigned n = 0; n <= BOOK2_TCU_OST; n++) {
        uint32_t hz = book2_tcu_hz(s, n);
        uint64_t ns = now - s->ch[n].epoch;
        uint64_t part, ticks, wrap;

        s->ch[n].epoch = now;
        if (!hz) {
            continue;
        }
        part = (ns % NANOSECONDS_PER_SECOND) * hz + s->ch[n].subtick;
        ticks = (ns / NANOSECONDS_PER_SECOND) * hz +
                part / NANOSECONDS_PER_SECOND;
        s->ch[n].subtick = part % NANOSECONDS_PER_SECOND;
        if (ticks >= book2_tcu_to_match(s, n, s->ch[n].full)) {
            s->tfr |= book2_tcu_bit(n);
        }
        if (n < BOOK2_TCU_CHANNELS && s->ch[n].half <= s->ch[n].full &&
            ticks >= book2_tcu_to_match(s, n, s->ch[n].half)) {
            s->tfr |= 1u << (16 + n);
        }
        wrap = book2_tcu_to_wrap(s, n);
        if (ticks >= wrap) {
            s->ch[n].count = (ticks - wrap) % book2_tcu_period(s, n);
        } else {
            s->ch[n].count += ticks;
        }
    }
}

static void book2_tcu_update_irq(Book2TcuState *s)
{
    uint32_t active = s->tfr & ~s->tmr;
    uint32_t channels = (active | (active >> 16)) & 0x3f;

    qemu_set_irq(s->irq[0], (active & BOOK2_TCU_OST_BIT) != 0);
    qemu_set_irq(s->irq[1], (channels & 0x20) != 0);
    qemu_set_irq(s->irq[2], (channels & 0x1f) != 0);
}

static void book2_tcu_arm(Book2TcuState *s, int64_t now)
{
    uint32_t notify = ~(s->tfr | s->tmr);
    int64_t next = INT64_MAX;

    for (unsigned n = 0; n <= BOOK2_TCU_OST; n++) {
        uint32_t hz = book2_tcu_hz(s, n);
        uint64_t ticks = UINT64_MAX;
        uint64_t ns;

        if (!hz) {
            continue;
        }
        if (notify & book2_tcu_bit(n)) {
            ticks = book2_tcu_to_match(s, n, s->ch[n].full);
        }
        if (n < BOOK2_TCU_CHANNELS && s->ch[n].half <= s->ch[n].full &&
            (notify & (1u << (16 + n)))) {
            ticks = MIN(ticks, book2_tcu_to_match(s, n, s->ch[n].half));
        }
        if (ticks == UINT64_MAX) {
            continue;
        }
        ns = (ticks * NANOSECONDS_PER_SECOND - s->ch[n].subtick + hz - 1) / hz;
        if (ns < (uint64_t)(next - now)) {
            next = now + ns;
        }
    }
    /* Masked or already-latched flags need no repeated host wakeups. Their
     * state and counter are updated on the next MMIO access or timer event. */
    if (next == INT64_MAX) {
        timer_del(s->event);
    } else {
        timer_mod(s->event, next);
    }
}

static void book2_tcu_expire(void *opaque)
{
    Book2TcuState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    book2_tcu_collect(s, now);
    book2_tcu_update_irq(s);
    book2_tcu_arm(s, now);
}

static uint64_t book2_tcu_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2TcuState *s = opaque;
    uint32_t val = 0;
    unsigned n;

    book2_tcu_collect(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    book2_tcu_update_irq(s);
    switch (addr & ~3ull) {
    case 0x10: val = s->ter; break;
    case 0x1c: val = s->tsr; break;
    case 0x20: val = s->tfr; break;
    case 0x30: val = s->tmr; break;
    case 0xe0: val = s->ch[BOOK2_TCU_OST].full; break;
    case 0xe8: val = s->ch[BOOK2_TCU_OST].count; break;
    case 0xec: val = s->ch[BOOK2_TCU_OST].csr; break;
    case 0xf0: val = (1u << 17) | (1u << 18); break; /* TCNT is current. */
    default:
        if (addr >= 0x40 && addr < 0xa0) {
            n = (addr - 0x40) / 0x10;
            switch (addr & 0x0c) {
            case 0x00: val = s->ch[n].full; break;
            case 0x04: val = s->ch[n].half; break;
            case 0x08: val = s->ch[n].count; break;
            case 0x0c: val = s->ch[n].csr; break;
            }
        }
        break;
    }
    return val >> (8 * (addr & 3));
}

static uint32_t book2_tcu_merge(uint32_t old, uint32_t val, uint32_t mask)
{
    return (old & ~mask) | (val & mask);
}

static void book2_tcu_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    Book2TcuState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned shift = 8 * (addr & 3), n;
    uint32_t mask = (uint32_t)((1ull << (size * 8)) - 1) << shift;
    uint32_t val = (uint32_t)data << shift;

    book2_tcu_collect(s, now);
    switch (addr & ~3ull) {
    case 0x10: s->ter = book2_tcu_merge(s->ter, val, mask & BOOK2_TCU_ENABLE_BITS); break;
    case 0x14: s->ter |= val & BOOK2_TCU_ENABLE_BITS; break;
    case 0x18: s->ter &= ~val; break;
    case 0x1c: s->tsr = book2_tcu_merge(s->tsr, val, mask & BOOK2_TCU_STOP_BITS); break;
    case 0x20: s->tfr = book2_tcu_merge(s->tfr, val, mask & BOOK2_TCU_FLAG_BITS); break;
    case 0x24: s->tfr |= val & BOOK2_TCU_FLAG_BITS; break;
    case 0x28: s->tfr &= ~val; break;
    case 0x2c: s->tsr |= val & BOOK2_TCU_STOP_BITS; break;
    case 0x30: s->tmr = book2_tcu_merge(s->tmr, val, mask); break;
    case 0x34: s->tmr |= val; break;
    case 0x38: s->tmr &= ~val; break;
    case 0x3c: s->tsr &= ~val; break;
    case 0xe0:
        n = BOOK2_TCU_OST;
        s->ch[n].full = book2_tcu_merge(s->ch[n].full, val, mask);
        break;
    case 0xe8:
        n = BOOK2_TCU_OST;
        s->ch[n].count = book2_tcu_merge(s->ch[n].count, val, mask);
        s->ch[n].subtick = 0;
        break;
    case 0xec:
        n = BOOK2_TCU_OST;
        s->ch[n].csr = book2_tcu_merge(s->ch[n].csr, val, mask & 0xffff);
        break;
    default:
        if (addr < 0x40 || addr >= 0xa0) {
            break;
        }
        n = (addr - 0x40) / 0x10;
        mask &= 0xffff;
        switch (addr & 0x0c) {
        case 0x00: s->ch[n].full = book2_tcu_merge(s->ch[n].full, val, mask); break;
        case 0x04: s->ch[n].half = book2_tcu_merge(s->ch[n].half, val, mask); break;
        case 0x08:
            s->ch[n].count = book2_tcu_merge(s->ch[n].count, val, mask);
            s->ch[n].subtick = 0;
            break;
        case 0x0c:
            s->ch[n].csr = book2_tcu_merge(s->ch[n].csr, val, mask);
            if ((n == 1 || n == 2) && (val & mask & (1u << 10))) {
                s->ch[n].count = 0;
                s->ch[n].subtick = 0;
                s->ch[n].csr &= ~(1u << 10);
            }
            break;
        }
        break;
    }
    book2_tcu_update_irq(s);
    book2_tcu_arm(s, now);
}

static const MemoryRegionOps book2_tcu_ops = {
    .read = book2_tcu_read,
    .write = book2_tcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void book2_tcu_realize(DeviceState *dev, Error **errp)
{
    Book2TcuState *s = BOOK2_TCU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* Retain the verified Book2 reset-mask compatibility value. */
    s->tmr = 0xfffdffddu;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_tcu_expire, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_tcu_ops, s,
                          "book2-tcu", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void book2_tcu_reset(DeviceState *dev)
{
    Book2TcuState *s = BOOK2_TCU(dev);

    timer_del(s->event);
    s->ter = s->tsr = s->tfr = 0;
    s->tmr = 0xfffdffddu;
    memset(s->ch, 0, sizeof(s->ch));
    book2_tcu_update_irq(s);
}

static void book2_tcu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_tcu_realize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, book2_tcu_reset);
}

static const TypeInfo book2_tcu_info = {
    .name = TYPE_BOOK2_TCU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2TcuState),
    .class_init = book2_tcu_class_init,
};
