/* Book2 JZ4750 AIC transmit FIFO + internal codec + QEMU audio backend.
 * Replace the complete old AIC section by #include "audio.inc.c".
 * This source uses the QEMU 11.1.3 tree's audio_be_* API (qemu/audio.h).
 * Hardware runs from virtual time; backend latency does not complete DMA.
 */
#include "qemu/audio.h"
#include "qemu/bswap.h"

#define TYPE_BOOK2_AIC "book2-aic"
OBJECT_DECLARE_SIMPLE_TYPE(Book2AicState, BOOK2_AIC)
#define B2A_FR       0x00
#define B2A_CR       0x04
#define B2A_I2SCR    0x10
#define B2A_SR       0x14
#define B2A_I2SSR    0x1c
#define B2A_DR       0x34
#define B2A_CKCFG    0xa0
#define B2A_RGADW    0xa4
#define B2A_RGDATA   0xa8
#define B2A_TDMS     (1u << 14)
#define B2A_REPLAY   (1u << 1)
#define B2A_M2S      (1u << 11)
#define B2A_ENDSW    (1u << 10)
#define B2A_AVSTSU   (1u << 9)
#define B2A_FLUSH    (1u << 8)
#define B2A_TFS      (1u << 3)
#define B2A_TUR      (1u << 5)
#define B2A_FIFO     32
#define B2A_PCM_SIZE (64 * 1024)
#define B2A_TICK_NS  100000LL
#define B2A_R(s, a)  ((s)->regs[(a) / 4])

struct Book2AicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint32_t regs[0x1000 / 4];
    uint8_t codec[128];
    uint32_t fifo[B2A_FIFO];
    unsigned fifo_head, fifo_count;
    int16_t last[2];
    uint8_t pcm[B2A_PCM_SIZE]; /* LE stereo S16, independent of host endianness */
    unsigned pcm_head, pcm_count;
    uint64_t sample_fraction;
    int64_t last_ns;
    unsigned rate;
    bool advancing;
};

static unsigned book2_aic_sample_bits(Book2AicState *s)
{
    static const unsigned bits[8] = { 8, 16, 18, 20, 24, 0, 0, 0 };
    return bits[(B2A_R(s, B2A_CR) >> 19) & 7];
}

static unsigned book2_aic_codec_rate(Book2AicState *s)
{
    static const unsigned rates[] = {
        96000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 9600, 8000,
    };
    unsigned code = s->codec[4] >> 4; /* DAC rate, firmware sets both nibbles */
    return code < ARRAY_SIZE(rates) ? rates[code] : 0;
}

static bool book2_aic_playing(Book2AicState *s)
{
    return (B2A_R(s, B2A_FR) & 0x11) == 0x11 &&
           (B2A_R(s, B2A_CR) & B2A_REPLAY) &&
           !(B2A_R(s, B2A_I2SCR) & (1u << 12)) &&
           book2_aic_sample_bits(s) && s->rate;
}

static bool book2_aic_codec_muted(Book2AicState *s)
{
    /* Internal codec power/mute register bits verified in factory jzdlv.c.
     * Analog gains/routes are retained in the register bank; no guessed EQ. */
    return !(B2A_R(s, B2A_FR) & (1u << 5)) ||
           (s->codec[1] & (1u << 5)) ||
           (s->codec[5] & ((1u << 7) | (1u << 6))) ||
           (s->codec[6] & 3);
}

static void book2_aic_update_irq(Book2AicState *s)
{
    uint32_t sr = B2A_R(s, B2A_SR) & (B2A_TUR | (1u << 6));
    uint32_t cr = B2A_R(s, B2A_CR);
    unsigned threshold = (B2A_R(s, B2A_FR) >> 8) & 15;
    bool codec_irq = (s->codec[9] & ~s->codec[8] & 0x3f) != 0;
    if (s->fifo_count <= threshold) {
        sr |= B2A_TFS;
    }
    B2A_R(s, B2A_SR) = sr | (s->fifo_count << 8);
    B2A_R(s, B2A_I2SSR) = book2_aic_playing(s) ? 4 : 0;
    qemu_set_irq(s->irq, ((sr & cr & 0x78) != 0) || codec_irq);
}

static void book2_aic_backend_callback(void *opaque, int available)
{
    Book2AicState *s = opaque;
    size_t bytes;
    while (available >= 4 && s->pcm_count >= 4 && s->voice) {
        bytes = MIN((unsigned)available, s->pcm_count);
        bytes = MIN(bytes, B2A_PCM_SIZE - s->pcm_head) & ~3u;
        bytes = audio_be_write(s->audio_be, s->voice, s->pcm + s->pcm_head, bytes);
        if (!bytes) {
            break;
        }
        s->pcm_head = (s->pcm_head + bytes) % B2A_PCM_SIZE;
        s->pcm_count -= bytes;
        available -= bytes;
    }
}

static void book2_aic_queue_frame(Book2AicState *s, int16_t l, int16_t r)
{
    unsigned tail;
    if (s->pcm_count > B2A_PCM_SIZE - 4) {
        /* An unavailable/slow host sink does not stop the physical I2S clock. */
        s->pcm_head = (s->pcm_head + 4) % B2A_PCM_SIZE;
        s->pcm_count -= 4;
    }
    tail = (s->pcm_head + s->pcm_count) % B2A_PCM_SIZE;
    stw_le_p(s->pcm + tail, l);
    stw_le_p(s->pcm + tail + 2, r);
    s->pcm_count += 4;
}

static int16_t book2_aic_pop_sample(Book2AicState *s, unsigned channel)
{
    uint32_t cr = B2A_R(s, B2A_CR), raw;
    unsigned bits = book2_aic_sample_bits(s);
    int32_t sample;
    if (!s->fifo_count) {
        B2A_R(s, B2A_SR) |= B2A_TUR;
        return (B2A_R(s, B2A_FR) & (1u << 6)) ? s->last[channel] : 0;
    }
    raw = s->fifo[s->fifo_head];
    s->fifo_head = (s->fifo_head + 1) % B2A_FIFO;
    s->fifo_count--;
    if (cr & B2A_ENDSW) {
        raw = bits == 16 ? bswap16(raw) : bits > 16 ? bswap32(raw) : raw;
    }
    raw &= (1u << bits) - 1;
    if (cr & B2A_AVSTSU) {
        raw ^= 1u << (bits - 1);
    }
    /* Sign extend a right-justified FIFO sample, then quantize for host S16. */
    sample = (int32_t)(raw << (32 - bits)) >> 16;
    s->last[channel] = sample;
    return sample;
}

static void book2_aic_advance(Book2AicState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed, scaled, frames;
    bool mono = B2A_R(s, B2A_CR) & B2A_M2S;
    if (s->advancing) {
        return;
    }
    s->advancing = true;
    if (!book2_aic_playing(s)) {
        s->last_ns = now;
        s->sample_fraction = 0;
        s->advancing = false;
        return;
    }
    elapsed = now >= s->last_ns ? now - s->last_ns : 0;
    /*
     * Only advance the sampling reference by the time actually consumed.
     * Folding the leftover sub-sample remainder into sample_fraction while
     * also moving last_ns to "now" would discard it; when the DMA engine
     * polls this function very frequently (every transfer unit) the elapsed
     * time per call is far below one sample period, so the FIFO would never
     * be drained and the guest would spin forever refilling it.
     */
    scaled = (elapsed % NANOSECONDS_PER_SECOND) * s->rate + s->sample_fraction;
    frames = (elapsed / NANOSECONDS_PER_SECOND) * s->rate +
             scaled / NANOSECONDS_PER_SECOND;
    s->sample_fraction = scaled % NANOSECONDS_PER_SECOND;
    s->last_ns = now - (int64_t)(s->sample_fraction / s->rate);
    /* Bound backend work after very large virtual-time jumps. The FIFO is
     * nevertheless fully drained, and underrun is recorded. */
    frames = MIN(frames, B2A_PCM_SIZE / 4);
    for (uint64_t i = 0; i < frames; i++) {
        int16_t l = book2_aic_pop_sample(s, 0);
        int16_t r = mono ? l : book2_aic_pop_sample(s, 1);
        if (book2_aic_codec_muted(s)) {
            l = r = 0;
        }
        book2_aic_queue_frame(s, l, r);
    }
    book2_aic_update_irq(s);
    s->advancing = false;
}

static void book2_aic_reconfigure(Book2AicState *s)
{
    unsigned rate = book2_aic_codec_rate(s);
    if (rate && (s->rate != rate || !s->voice)) {
        struct audsettings as = {
            .freq = rate,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };
        s->voice = audio_be_open_out(s->audio_be, s->voice, "book2-icdc",
                                     s, book2_aic_backend_callback, &as);
        s->pcm_head = s->pcm_count = 0;
        s->sample_fraction = 0;
    }
    s->rate = rate;
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, book2_aic_playing(s));
    }
    if (book2_aic_playing(s)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + B2A_TICK_NS);
    } else {
        timer_del(s->timer);
    }
    book2_aic_update_irq(s);
}

static void book2_aic_tick(void *opaque)
{
    Book2AicState *s = opaque;
    book2_aic_advance(s);
    if (book2_aic_playing(s)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + B2A_TICK_NS);
    }
}

/* DMA bank1 channel1 uses req24, fixed DR destination, one sample per bus beat.
 * writes is transfer-unit bytes / destination beat width, NOT raw byte count. */
static bool book2_aic_dma_ready(DeviceState *dev, unsigned writes)
{
    Book2AicState *s = BOOK2_AIC(dev);
    unsigned threshold;
    book2_aic_advance(s);
    threshold = (B2A_R(s, B2A_FR) >> 8) & 15;
    return (B2A_R(s, B2A_FR) & 1) && (B2A_R(s, B2A_CR) & B2A_TDMS) &&
           s->fifo_count <= threshold && writes <= B2A_FIFO - s->fifo_count;
}

static uint64_t book2_aic_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2AicState *s = opaque;
    uint32_t value;
    if (addr >= sizeof(s->regs) || (addr & 3) + size > 4) {
        return 0;
    }
    book2_aic_advance(s);
    book2_aic_update_irq(s);
    if ((addr & ~3u) == B2A_RGDATA) {
        unsigned index = (B2A_R(s, B2A_RGADW) >> 8) & 127;
        value = s->codec[index];
        if (s->codec[9] & ~s->codec[8] & 0x3f) {
            value |= 1u << 8;
        }
    } else if ((addr & ~3u) == B2A_DR) {
        value = 0; /* Recording is not wired; no invented capture samples. */
    } else {
        value = B2A_R(s, addr & ~3u);
    }
    return (value >> ((addr & 3) * 8)) & (UINT32_MAX >> ((4 - size) * 8));
}

static void book2_aic_codec_write(Book2AicState *s, unsigned index, uint8_t value)
{
    uint8_t old = s->codec[index];
    if (index == 9) {
        s->codec[9] &= ~value; /* ICDC IFR is W1C, unlike LCD STATE. */
    } else {
        s->codec[index] = value;
        /* Output power ramp and DAC mute transitions complete immediately.
         * Their hardware completion flags remain latched until IFR W1C. */
        if (index == 5 && ((old ^ value) & (1u << 6))) {
            s->codec[9] |= value & (1u << 6) ? 4 : 8;
        }
        if (index == 1 && ((old ^ value) & (1u << 5))) {
            s->codec[9] |= value & (1u << 5) ? 1 : 2;
        }
    }
}

static void book2_aic_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Book2AicState *s = opaque;
    unsigned off = addr & ~3u, shift = (addr & 3) * 8;
    uint32_t old, val, mask;
    if (addr >= sizeof(s->regs) || (addr & 3) + size > 4) {
        return;
    }
    book2_aic_advance(s);
    old = B2A_R(s, off);
    mask = (UINT32_MAX >> ((4 - size) * 8)) << shift;
    val = (old & ~mask) | (((uint32_t)value << shift) & mask);
    switch (off) {
    case B2A_DR:
        if (s->fifo_count < B2A_FIFO) {
            s->fifo[(s->fifo_head + s->fifo_count) % B2A_FIFO] = (uint32_t)value;
            s->fifo_count++;
        }
        break;
    case B2A_SR:
        /* Error flags are W0C. FIFO counts/service flags are hardware-owned. */
        B2A_R(s, off) = old & (val | ~mask) & (B2A_TUR | (1u << 6));
        break;
    case B2A_I2SSR:
    case B2A_RGDATA:
        break;
    case B2A_CKCFG:
        B2A_R(s, off) = val | 2; /* internal codec clock ready */
        break;
    case B2A_RGADW:
        B2A_R(s, off) = val & ~(1u << 16); /* RGWR busy self-clears */
        if (val & (1u << 16)) {
            book2_aic_codec_write(s, (val >> 8) & 127, val & 255);
        }
        break;
    case B2A_FR:
        B2A_R(s, off) = val & ~(1u << 3); /* AIC reset pulse */
        if (val & (1u << 3)) {
            s->fifo_head = s->fifo_count = 0;
            B2A_R(s, B2A_CR) = 0;
            B2A_R(s, B2A_SR) = 0;
            memset(s->last, 0, sizeof(s->last));
        }
        break;
    case B2A_CR:
        B2A_R(s, off) = val & ~B2A_FLUSH;
        if (val & B2A_FLUSH) {
            s->fifo_head = s->fifo_count = 0;
            B2A_R(s, B2A_SR) &= ~B2A_TUR;
        }
        break;
    default:
        B2A_R(s, off) = val;
        break;
    }
    if (off == B2A_FR || off == B2A_CR || off == B2A_I2SCR || off == B2A_RGADW) {
        book2_aic_reconfigure(s);
    }
    book2_aic_update_irq(s);
}

static const MemoryRegionOps book2_aic_ops = {
    .read = book2_aic_read,
    .write = book2_aic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void book2_aic_reset(DeviceState *dev)
{
    Book2AicState *s = BOOK2_AIC(dev);
    timer_del(s->timer);
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->codec, 0, sizeof(s->codec));
    memset(s->last, 0, sizeof(s->last));
    s->codec[1] = 0x30; /* DAC muted, headphone disabled */
    s->codec[4] = 0x11; /* 48kHz DAC/ADC default */
    s->codec[5] = 0xff; /* analog blocks in standby */
    s->codec[6] = 3;    /* global standby + sleep */
    s->codec[8] = 0x3f;
    B2A_R(s, B2A_CKCFG) = 2;
    s->fifo_head = s->fifo_count = s->pcm_head = s->pcm_count = 0;
    s->sample_fraction = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rate = 0; /* Force a matching backend voice on the next configuration. */
    book2_aic_update_irq(s);
}

static void book2_aic_realize(DeviceState *dev, Error **errp)
{
    Book2AicState *s = BOOK2_AIC(dev);
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_aic_tick, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_aic_ops, s,
                          "book2-aic", sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    book2_aic_reset(dev);
}

static void book2_aic_finalize(Object *obj)
{
    Book2AicState *s = BOOK2_AIC(obj);
    if (s->timer) {
        timer_free(s->timer);
    }
    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
    }
}

static const Property book2_aic_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Book2AicState, audio_be),
};

static void book2_aic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_aic_realize;
    device_class_set_legacy_reset(dc, book2_aic_reset);
    device_class_set_props(dc, book2_aic_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo book2_aic_info = {
    .name = TYPE_BOOK2_AIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2AicState),
    .instance_finalize = book2_aic_finalize,
    .class_init = book2_aic_class_init,
};
