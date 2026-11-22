/* Book2 board GPIO/keyboard, RTC and SADC. Included by book2.c. */
#include "ui/input.h"
#include "hw/core/qdev-properties.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/system.h"

#define TYPE_BOOK2_GPIO "book2-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Book2GpioState, BOOK2_GPIO)

#define BOOK2_GPIO_BANKS 6
#define BOOK2_KEY_SENSE 11
#define BOOK2_KEY_DRIVE 5
#define BOOK2_GPIO_USB_VBUS 144

/* data2.dat.bin: sense pins at 0x80102b90, drive pins at 0x80102bbc. */
static const uint8_t book2_key_sense[BOOK2_KEY_SENSE] = {
    131, 130, 134, 137, 132, 136, 133, 152, 153, 183, 181,
};
static const uint8_t book2_key_drive[BOOK2_KEY_DRIVE] = {128, 129, 138, 84, 139};
/* The firmware translates a closure to index (4-drive)*11 + 10-sense.
 * Entries use Linux/AT key numbers, as supplied by QEMU's input API. */
static const uint8_t book2_key_code[BOOK2_KEY_DRIVE * BOOK2_KEY_SENSE] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
    0x3a, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x32, 0x33, 0x68, 0x67, 0x6d, 0x1b,
    0x2b, 0x35, 0x28, 0x39, 0x31, 0x77, 0x69, 0x6c, 0x6a, 0x1c, 0x6f,
};

struct Book2GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[BOOK2_GPIO_BANKS];
    QemuInputHandlerState *keyboard;
    QEMUTimer *key_release;
    struct {
        uint32_t pins, data, mask, pull, func, sel, dir, trig, flag;
    } g[BOOK2_GPIO_BANKS];
    bool pressed[BOOK2_KEY_DRIVE * BOOK2_KEY_SENSE];
    int64_t release_at[BOOK2_KEY_DRIVE * BOOK2_KEY_SENSE];
    uint32_t external_low[BOOK2_GPIO_BANKS];
};

static uint32_t book2_gpio_outputs(Book2GpioState *s, unsigned bank)
{
    return s->g[bank].dir & ~(s->g[bank].func | s->g[bank].sel);
}

static bool book2_gpio_driven_low(Book2GpioState *s, unsigned pin)
{
    unsigned bank = pin / 32;
    return (book2_gpio_outputs(s, bank) & ~s->g[bank].data &
            (1u << (pin % 32))) != 0;
}

static void book2_gpio_update(Book2GpioState *s)
{
    uint32_t low[BOOK2_GPIO_BANKS];

    memcpy(low, s->external_low, sizeof(low));
    for (unsigned drive = 0; drive < BOOK2_KEY_DRIVE; drive++) {
        if (!book2_gpio_driven_low(s, book2_key_drive[drive])) {
            continue;
        }
        for (unsigned sense = 0; sense < BOOK2_KEY_SENSE; sense++) {
            unsigned index = (BOOK2_KEY_DRIVE - 1 - drive) * BOOK2_KEY_SENSE +
                             (BOOK2_KEY_SENSE - 1 - sense);
            unsigned pin = book2_key_sense[sense];

            if (s->pressed[index]) {
                low[pin / 32] |= 1u << (pin % 32);
            }
        }
    }
    for (unsigned bank = 0; bank < BOOK2_GPIO_BANKS; bank++) {
        uint32_t outputs = book2_gpio_outputs(s, bank);
        uint32_t pins = ((~low[bank]) & ~outputs) | (s->g[bank].data & outputs);
        uint32_t intr = ~s->g[bank].func & s->g[bank].sel;
        uint32_t active = ~(pins ^ s->g[bank].dir);
        uint32_t changed = pins ^ s->g[bank].pins;
        uint32_t edge = intr & s->g[bank].trig;
        uint32_t level = intr & ~s->g[bank].trig;

        s->g[bank].pins = pins;
        s->g[bank].flag |= changed & active & edge;
        qemu_set_irq(s->irq[bank],
                     ((s->g[bank].flag | (level & active)) & intr &
                      ~s->g[bank].mask) != 0);
    }
}

static void book2_gpio_external(void *opaque, int pin, int level)
{
    Book2GpioState *s = opaque;
    uint32_t bit = 1u << (pin % 32);

    if (level) {
        s->external_low[pin / 32] &= ~bit;
    } else {
        s->external_low[pin / 32] |= bit;
    }
    book2_gpio_update(s);
}

/*
 * The firmware scans the matrix roughly every 10-20 ms of guest time and
 * debounces a closure over several consecutive scans.  A quick host
 * tap can be shorter than that window, in which case the key would never
 * be observed and the press appears lost.  Hold each closure for a short
 * minimum before honouring the release, exactly like a physical key that
 * cannot be pressed for zero time.
 */
#define BOOK2_KEY_MIN_HOLD_NS (40 * 1000 * 1000LL)

static void book2_gpio_key_set(Book2GpioState *s, unsigned index, bool down);

static void book2_gpio_key_release(void *opaque)
{
    Book2GpioState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = INT64_MAX;

    for (unsigned i = 0; i < ARRAY_SIZE(s->pressed); i++) {
        if (!s->release_at[i]) {
            continue;
        }
        if (s->release_at[i] <= now) {
            s->release_at[i] = 0;
            book2_gpio_key_set(s, i, false);
        } else {
            next = MIN(next, s->release_at[i]);
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->key_release, next);
    }
}

static void book2_gpio_key_set(Book2GpioState *s, unsigned index, bool down)
{
    s->pressed[index] = down;
    book2_gpio_update(s);
}

static void book2_gpio_key(DeviceState *dev, QemuConsole *src,
                          QemuInputEvent *evt)
{
    Book2GpioState *s = BOOK2_GPIO(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(book2_key_code); i++) {
        if (book2_key_code[i] != evt->key.key) {
            continue;
        }
        if (evt->key.down) {
            s->release_at[i] = 0;
            book2_gpio_key_set(s, i, true);
        } else if (s->pressed[i]) {
            if (now < s->release_at[i] || !s->release_at[i]) {
                /* Newly pressed: guarantee a hold long enough to be scanned. */
                s->release_at[i] = now + BOOK2_KEY_MIN_HOLD_NS;
                timer_mod(s->key_release, s->release_at[i]);
            }
        }
        break;
    }
}

static const QemuInputHandler book2_gpio_keys = {
    .name = "Book2 keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = book2_gpio_key,
};

static uint64_t book2_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2GpioState *s = opaque;
    unsigned bank = addr >> 8;
    uint32_t intr = ~s->g[bank].func & s->g[bank].sel;
    uint32_t level = intr & ~s->g[bank].trig &
                     ~(s->g[bank].pins ^ s->g[bank].dir);

    switch (addr & 0xff) {
    case 0x00: return s->g[bank].pins;
    case 0x10: return s->g[bank].data;
    case 0x20: return s->g[bank].mask;
    case 0x30: return s->g[bank].pull;
    case 0x40: return s->g[bank].func;
    case 0x50: return s->g[bank].sel;
    case 0x60: return s->g[bank].dir;
    case 0x70: return s->g[bank].trig;
    case 0x80: return s->g[bank].flag | level;
    default: return 0;
    }
}

static void book2_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Book2GpioState *s = opaque;
    unsigned bank = addr >> 8;
    unsigned off = addr & 0xff;
    uint32_t *reg;

    switch (off & 0xf0) {
    case 0x10: reg = &s->g[bank].data; break;
    case 0x20: reg = &s->g[bank].mask; break;
    case 0x30: reg = &s->g[bank].pull; break;
    case 0x40: reg = &s->g[bank].func; break;
    case 0x50: reg = &s->g[bank].sel; break;
    case 0x60: reg = &s->g[bank].dir; break;
    case 0x70: reg = &s->g[bank].trig; break;
    default: return;
    }
    switch (off & 0x0f) {
    case 0:
        *reg = val;
        break;
    case 4:
        if (off == 0x14) {
            /* PXFLGC shares PXDATS and acknowledges interrupt-mode pins. */
            uint32_t intr = s->g[bank].sel & ~s->g[bank].func;
            s->g[bank].flag &= ~((uint32_t)val & intr);
            val &= ~intr;
        }
        *reg |= val;
        break;
    case 8:
        *reg &= ~val;
        break;
    default:
        return;
    }
    book2_gpio_update(s);
}

static const MemoryRegionOps book2_gpio_ops = {
    .read = book2_gpio_read,
    .write = book2_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void book2_gpio_reset(DeviceState *dev)
{
    Book2GpioState *s = BOOK2_GPIO(dev);

    memset(s->g, 0, sizeof(s->g));
    memset(s->pressed, 0, sizeof(s->pressed));
    memset(s->release_at, 0, sizeof(s->release_at));
    if (s->key_release) {
        timer_del(s->key_release);
    }
    for (unsigned n = 0; n < BOOK2_GPIO_BANKS; n++) {
        s->g[n].mask = UINT32_MAX;
    }
    book2_gpio_update(s);
}

static void book2_gpio_realize(DeviceState *dev, Error **errp)
{
    Book2GpioState *s = BOOK2_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_gpio_ops, s,
                          "book2-gpio", BOOK2_GPIO_BANKS * 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned n = 0; n < BOOK2_GPIO_BANKS; n++) {
        sysbus_init_irq(sbd, &s->irq[n]);
    }
    qdev_init_gpio_in(dev, book2_gpio_external, BOOK2_GPIO_BANKS * 32);
    /* PE16 is USB VBUS detect. Default to no external host attached. */
    s->external_low[BOOK2_GPIO_USB_VBUS / 32] |=
        1u << (BOOK2_GPIO_USB_VBUS % 32);
    s->keyboard = qemu_input_handler_register(dev, &book2_gpio_keys);
    s->key_release = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_gpio_key_release, s);
    book2_gpio_reset(dev);
}

static void book2_gpio_unrealize(DeviceState *dev)
{
    Book2GpioState *s = BOOK2_GPIO(dev);

    qemu_input_handler_unregister(s->keyboard);
    if (s->key_release) {
        timer_free(s->key_release);
        s->key_release = NULL;
    }
}

static void book2_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = book2_gpio_realize;
    dc->unrealize = book2_gpio_unrealize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, book2_gpio_reset);
}

static const TypeInfo book2_gpio_info = {
    .name = TYPE_BOOK2_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2GpioState),
    .class_init = book2_gpio_class_init,
};

#define TYPE_BOOK2_RTC "book2-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Book2RtcState, BOOK2_RTC)

#define BOOK2_RTC_WRDY 0x80u
#define BOOK2_RTC_1HZ  0x40u
#define BOOK2_RTC_1HZE 0x20u
#define BOOK2_RTC_AF   0x10u
#define BOOK2_RTC_AIE  0x08u
#define BOOK2_RTC_AE   0x04u
#define BOOK2_RTC_EN   0x01u

struct Book2RtcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq;
    int64_t next_tick;
    uint32_t reg[0x38 / 4];
    char *state_file;
    QEMUTimer *persist_timer;
    Notifier exit_notifier;
    bool persist_error_reported;
};

/* Versioned little-endian battery-domain image; no guest addresses/state. */
#define BOOK2_RTC_STATE_PAYLOAD 80
#define BOOK2_RTC_STATE_SIZE (BOOK2_RTC_STATE_PAYLOAD + 32)
#define BOOK2_RTC_CHECKPOINT_NS (60LL * NANOSECONDS_PER_SECOND)

static uint32_t book2_rtc_battery_reg(Book2RtcState *s, unsigned n)
{
    if (n == 0) {
        /* IRQ status and synthesized WRDY are not battery configuration. */
        return s->reg[n] & ~(BOOK2_RTC_WRDY | BOOK2_RTC_AF | BOOK2_RTC_1HZ);
    }
    return (n >= 4 && n < 8) ? 0 : s->reg[n];
}

static void book2_rtc_state_digest(const uint8_t *data, uint8_t *digest)
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize len = 32;

    g_checksum_update(sum, data, BOOK2_RTC_STATE_PAYLOAD);
    g_checksum_get_digest(sum, digest, &len);
    g_checksum_free(sum);
}

static bool book2_rtc_save(Book2RtcState *s, Error **errp)
{
    uint8_t data[BOOK2_RTC_STATE_SIZE] = { 0 };
    g_autoptr(GError) err = NULL;

    if (!s->state_file) {
        return true;
    }
    memcpy(data, "BK2RTC01", 8);
    stl_le_p(data + 8, 1);
    stl_le_p(data + 12, ARRAY_SIZE(s->reg));
    stq_le_p(data + 16, g_get_real_time() / G_USEC_PER_SEC);
    for (unsigned n = 0; n < ARRAY_SIZE(s->reg); n++) {
        stl_le_p(data + 24 + n * 4, book2_rtc_battery_reg(s, n));
    }
    book2_rtc_state_digest(data, data + BOOK2_RTC_STATE_PAYLOAD);
    if (!g_file_set_contents_full(s->state_file, (const char *)data,
                                 sizeof(data),
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_DURABLE, 0600, &err)) {
        error_setg(errp, "book2-rtc: cannot save state-file '%s': %s",
                   s->state_file, err->message);
        return false;
    }
    return true;
}

static bool book2_rtc_load(Book2RtcState *s, Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) err = NULL;
    uint8_t digest[32];
    const uint8_t *data;
    gsize len = 0;
    int64_t saved, now;

    if (!s->state_file) {
        return true;
    }
    if (!*s->state_file) {
        error_setg(errp, "book2-rtc: state-file must not be empty");
        return false;
    }
    if (!g_file_get_contents(s->state_file, &contents, &len, &err)) {
        if (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            return true; /* First cold boot retains real battery defaults. */
        }
        error_setg(errp, "book2-rtc: cannot read state-file '%s': %s",
                   s->state_file, err->message);
        return false;
    }
    data = (const uint8_t *)contents;
    if (len != BOOK2_RTC_STATE_SIZE || memcmp(data, "BK2RTC01", 8) ||
        ldl_le_p(data + 8) != 1 ||
        ldl_le_p(data + 12) != ARRAY_SIZE(s->reg)) {
        error_setg(errp, "book2-rtc: invalid state-file '%s' (format/version)",
                   s->state_file);
        return false;
    }
    book2_rtc_state_digest(data, digest);
    if (memcmp(digest, data + BOOK2_RTC_STATE_PAYLOAD, sizeof(digest))) {
        error_setg(errp, "book2-rtc: invalid state-file '%s' (checksum)",
                   s->state_file);
        return false;
    }
    saved = ldq_le_p(data + 16);
    if (saved < 0) {
        error_setg(errp, "book2-rtc: invalid state-file '%s' (timestamp)",
                   s->state_file);
        return false;
    }
    for (unsigned n = 0; n < ARRAY_SIZE(s->reg); n++) {
        s->reg[n] = ldl_le_p(data + 24 + n * 4);
    }
    s->reg[0] = book2_rtc_battery_reg(s, 0);
    for (unsigned n = 4; n < 8; n++) {
        s->reg[n] = 0;
    }
    now = g_get_real_time() / G_USEC_PER_SEC;
    if ((s->reg[0] & BOOK2_RTC_EN) && now > saved) {
        /* Hardware seconds are 32-bit and intentionally wrap. A backwards
         * host clock never winds the guest clock backwards. */
        s->reg[1] += (uint32_t)(now - saved);
    }
    return true;
}

static void book2_rtc_irq(Book2RtcState *s)
{
    uint32_t ctrl = s->reg[0];
    qemu_set_irq(s->irq, ((ctrl & BOOK2_RTC_1HZ) && (ctrl & BOOK2_RTC_1HZE)) ||
                        ((ctrl & BOOK2_RTC_AF) && (ctrl & BOOK2_RTC_AIE)));
}

static void book2_rtc_update(Book2RtcState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if ((s->reg[0] & BOOK2_RTC_EN) && now >= s->next_tick) {
        uint64_t ticks = 1 + (now - s->next_tick) / NANOSECONDS_PER_SECOND;
        uint32_t delta = s->reg[2] - s->reg[1];

        if ((s->reg[0] & BOOK2_RTC_AE) &&
            (delta ? delta : (1ull << 32)) <= ticks) {
            s->reg[0] |= BOOK2_RTC_AF;
        }
        s->reg[1] += ticks;
        s->reg[0] |= BOOK2_RTC_1HZ;
        s->next_tick += ticks * NANOSECONDS_PER_SECOND;
    }
    book2_rtc_irq(s);
}

static void book2_rtc_flush(Book2RtcState *s)
{
    Error *err = NULL;

    book2_rtc_update(s);
    if (!book2_rtc_save(s, &err)) {
        if (!s->persist_error_reported) {
            error_report_err(err);
            s->persist_error_reported = true;
        } else {
            error_free(err);
        }
    } else {
        s->persist_error_reported = false;
    }
}

static void book2_rtc_checkpoint(void *opaque)
{
    Book2RtcState *s = opaque;

    if (s->reg[0] & BOOK2_RTC_EN) {
        book2_rtc_flush(s);
    }
    timer_mod(s->persist_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              BOOK2_RTC_CHECKPOINT_NS);
}

static void book2_rtc_exit(Notifier *n, void *unused)
{
    Book2RtcState *s = container_of(n, Book2RtcState, exit_notifier);

    book2_rtc_flush(s);
}

static void book2_rtc_tick(void *opaque)
{
    Book2RtcState *s = opaque;

    book2_rtc_update(s);
    if (s->reg[0] & BOOK2_RTC_EN) {
        timer_mod(s->timer, s->next_tick);
    }
}

static uint64_t book2_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2RtcState *s = opaque;

    if (addr >= sizeof(s->reg)) {
        return 0;
    }
    book2_rtc_update(s);
    return s->reg[addr / 4] | (addr == 0 ? BOOK2_RTC_WRDY : 0);
}

static void book2_rtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Book2RtcState *s = opaque;
    uint32_t old_ctrl = s->reg[0];
    uint32_t old_battery;

    book2_rtc_update(s);
    if (addr >= sizeof(s->reg) || (addr & 3)) {
        return;
    }
    old_battery = book2_rtc_battery_reg(s, addr / 4);
    switch (addr) {
    case 0x00: {
        uint32_t flags = BOOK2_RTC_AF | BOOK2_RTC_1HZ;
        s->reg[0] = (val & ~(flags | BOOK2_RTC_WRDY)) | (s->reg[0] & val & flags);
        break;
    }
    case 0x04: /* Seconds. */
    case 0x08: /* Alarm comparator. */
    case 0x0c: /* Regulator. */
    case 0x20: /* Hibernate control. */
    case 0x24: /* Wake filter. */
    case 0x28: /* Reset counter. */
    case 0x2c: /* Wake control. */
    case 0x30: /* Wake status. */
    case 0x34: /* Battery-backed scratch. */
        s->reg[addr / 4] = val;
        break;
    default:
        return;
    }
    if (!(s->reg[0] & BOOK2_RTC_EN)) {
        timer_del(s->timer);
    } else if (!(old_ctrl & BOOK2_RTC_EN) || addr == 0x04) {
        s->next_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND;
        timer_mod(s->timer, s->next_tick);
    }
    book2_rtc_irq(s);
    if (s->state_file &&
        old_battery != book2_rtc_battery_reg(s, addr / 4)) {
        /* Commit guest battery-domain writes, including seconds and HSPR.
         * Mere 1-Hz/IRQ acknowledgements do not cause disk writes. */
        book2_rtc_flush(s);
    }
}

static const MemoryRegionOps book2_rtc_ops = {
    .read = book2_rtc_read,
    .write = book2_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void book2_rtc_realize(DeviceState *dev, Error **errp)
{
    Book2RtcState *s = BOOK2_RTC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* Battery-domain defaults. Boot0 advances HSPR[31:30] to select
     * a redundant copy. Start at 2 so the first cold boot selects 0. */
    s->reg[0x34 / 4] = 0x80000000u;
    s->reg[0x0c / 4] = 32767;
    if (!book2_rtc_load(s, errp) || !book2_rtc_save(s, errp)) {
        /* Fail early on corrupt or unwritable files; never silently discard
         * battery state or forge the firmware's initialized-time marker. */
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_rtc_tick, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_rtc_ops, s,
                          "book2-rtc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    if (s->reg[0] & BOOK2_RTC_EN) {
        s->next_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       NANOSECONDS_PER_SECOND;
        timer_mod(s->timer, s->next_tick);
    }
    if (s->state_file) {
        s->persist_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                        book2_rtc_checkpoint, s);
        timer_mod(s->persist_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                  BOOK2_RTC_CHECKPOINT_NS);
        s->exit_notifier.notify = book2_rtc_exit;
        qemu_add_exit_notifier(&s->exit_notifier);
    }
}

static void book2_rtc_unrealize(DeviceState *dev)
{
    Book2RtcState *s = BOOK2_RTC(dev);

    if (s->state_file) {
        qemu_remove_exit_notifier(&s->exit_notifier);
        timer_free(s->persist_timer);
        book2_rtc_flush(s);
    }
    timer_free(s->timer);
}

static const Property book2_rtc_properties[] = {
    DEFINE_PROP_STRING("state-file", Book2RtcState, state_file),
};

static void book2_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = book2_rtc_realize;
    dc->unrealize = book2_rtc_unrealize;
    device_class_set_props(dc, book2_rtc_properties);
    dc->user_creatable = false;
}

static const TypeInfo book2_rtc_info = {
    .name = TYPE_BOOK2_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2RtcState),
    .class_init = book2_rtc_class_init,
};

#define TYPE_BOOK2_SADC "book2-sadc"
OBJECT_DECLARE_SIMPLE_TYPE(Book2SadcState, BOOK2_SADC)

struct Book2SadcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t reg[0x2c / 4];
};

static void book2_sadc_irq(Book2SadcState *s)
{
    qemu_set_irq(s->irq, (s->reg[3] & ~s->reg[2] & 0x3f) != 0);
}

static void book2_sadc_done(void *opaque)
{
    Book2SadcState *s = opaque;
    uint32_t conversions = s->reg[0] & 3;

    if (conversions & 2) {
        /* Board default 4.0 V at the battery terminal. The firmware uses
         * two voltage ranges (cfg bit4), with a 12-bit result. */
        s->reg[0x1c / 4] = (s->reg[1] & 0x10) ? 3277 : 1638;
    }
    s->reg[0] &= ~conversions;
    s->reg[3] |= conversions;
    book2_sadc_irq(s);
}

static uint64_t book2_sadc_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2SadcState *s = opaque;
    return s->reg[addr / 4] >> (8 * (addr & 3));
}

static void book2_sadc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Book2SadcState *s = opaque;
    unsigned offset = addr & ~3u;
    unsigned shift = 8 * (addr & 3);
    uint32_t *reg = &s->reg[addr / 4];
    uint32_t mask = (uint32_t)((1ull << (size * 8)) - 1) << shift;
    uint32_t old = *reg;

    val <<= shift;
    switch (offset) {
    case 0x0c: /* Conversion and pen flags are W1C. */
        *reg &= ~(val & mask & 0x3f);
        break;
    case 0x00:
    case 0x08:
        mask &= 0xff;
        *reg = (old & ~mask) | (val & mask);
        if (offset == 0) {
            if (!(*reg & 3)) {
                timer_del(s->timer);
            } else if (!timer_pending(s->timer)) {
                timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
        break;
    case 0x10:
    case 0x14:
        mask &= 0xffff;
        /* fall through */
    case 0x04:
    case 0x28:
        *reg = (old & ~mask) | (val & mask);
        break;
    default:
        return;
    }
    book2_sadc_irq(s);
}

static const MemoryRegionOps book2_sadc_ops = {
    .read = book2_sadc_read,
    .write = book2_sadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void book2_sadc_reset(DeviceState *dev)
{
    Book2SadcState *s = BOOK2_SADC(dev);

    timer_del(s->timer);
    memset(s->reg, 0, sizeof(s->reg));
    s->reg[2] = 0x3f;
    book2_sadc_irq(s);
}

static void book2_sadc_realize(DeviceState *dev, Error **errp)
{
    Book2SadcState *s = BOOK2_SADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, book2_sadc_done, s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_sadc_ops, s,
                          "book2-sadc", 0x2c);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    book2_sadc_reset(dev);
}

static void book2_sadc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = book2_sadc_realize;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, book2_sadc_reset);
}

static const TypeInfo book2_sadc_info = {
    .name = TYPE_BOOK2_SADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2SadcState),
    .class_init = book2_sadc_class_init,
};
