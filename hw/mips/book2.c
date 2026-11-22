/*
 * QEMU "Book2" machine 鈥?BBK (EEBBK) Learning Computer Book 2
 *
 * SoC: Ingenic JZ4750 (XBurst, MIPS32r2 little-endian)
 * Board facts (reverse-engineered from the original firmware, see the
 * project's investigation report):
 *   - 64 MB SDRAM @ phys 0, firmware executes in kseg0 (0x80000000...)
 *   - LCD framebuffer at phys 0x03D00000, 800x480 RGB565 (inside RAM)
 *   - Debug console: UART3 @ 0x10033000, 16550 layout with 4-byte
 *     register stride (regshift 2)
 *   - Storage: SD cards on MSC0 (0x10021000, system card read by Boot0)
 *     and MSC1 (0x10022000, content card with the FAT32 A:/B: volumes)
 *   - Boot flow: mask-ROM-equivalent loads the first stage (data0,
 *     "Boot0", 5608 bytes) from LBA 0 of the MSC0 card to 0x80000000;
 *     Boot0 enumerates the card (CMD0/8/55/ACMD41/CMD2/3/9/7/55/ACMD6)
 *     then reads the OS image (tinyos) from the LBA copy table at
 *     0x800015C0 (0x800 / 0x7800 / 0x10000) to 0x80005000 and jumps.
 *
 * Register semantics implemented from the Ingenic factory header
 * (linux-2.6.24.3-NP1100 include/asm-mips/mach-jz4750/regs.h, obtained
 * from the OpenNoah/virtualnoah project) cross-checked against
 * disassembly of the Book2 firmware itself.
 *
 * The SD-card model is a faithful port of the behaviour-validated
 * python model from the earlier unicorn-based emulator (jz4750.py):
 * response framing (raw frames read out through the 16-bit RES
 * register), RXFIFO byte order, STAT/IREG bit timing, CSD v1 layout.
 *
 * Design rule (project constraint): this machine models hardware
 * only.  It never inspects or alters guest program state; the guest
 * firmware reads everything (OS image, FAT32 resources) through its
 * own drivers from the block devices presented below.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "ui/console.h"
#include "hw/char/serial-mm.h"
#include "system/system.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "target/mips/cpu.h"
#include "qom/object.h"
#include "trace/trace-hw_mips.h"

/* ------------------------------------------------------------------ */
/* JZ4750 hardware map (physical addresses)                            */

#define BOOK2_RAM_SIZE          (64 * MiB)

#define JZ_CPM_BASE             0x10000000
#define JZ_INTC_BASE            0x10001000
#define JZ_TCU_BASE             0x10002000
#define JZ_RTC_BASE             0x10003000
#define JZ_GPIO_BASE            0x10010000
#define JZ_AIC_BASE             0x10020000
#define JZ_MSC0_BASE            0x10021000
#define JZ_MSC1_BASE            0x10022000
#define JZ_UART_BASE            0x10030000   /* UART0..3, step 0x1000 */
#define JZ_SADC_BASE            0x10070000
#define JZ_EMC_BASE             0x13010000
#define JZ_DMAC_BASE            0x13020000
#define JZ_UHC_BASE             0x13030000
#define JZ_LCD_BASE             0x13050000

/* JZ4750 INTC line numbers (factory regs.h) */
#define JZ_IRQ_UART3            3
#define JZ_IRQ_UART2            4
#define JZ_IRQ_UART1            5
#define JZ_IRQ_UART0            6
#define JZ_IRQ_MSC1             13
#define JZ_IRQ_MSC0             14
#define JZ_IRQ_UHC              17
#define JZ_IRQ_DMAC             20

/* JZ4750 MSC register offsets (factory regs.h) */
#define MSC_STRPCL              0x000   /* 16-bit: RESET=0x8, START_OP=0x4 */
#define MSC_STAT                0x004   /* 32-bit status */
#define MSC_CLKRT               0x008   /* 16-bit clock rate */
#define MSC_CMDAT               0x00c   /* 32-bit command attributes */
#define MSC_CMDAT_DATA_EN        (1u << 3)
#define MSC_RESTO               0x010
#define MSC_RDTO                0x014
#define MSC_BLKLEN              0x018   /* 16-bit block length */
#define MSC_NOB                 0x01c   /* 16-bit number of blocks */
#define MSC_SNOB                0x020
#define MSC_IMASK               0x024   /* 16-bit interrupt mask */
#define MSC_IREG                0x028   /* 16-bit interrupt flags, w1c */
#define MSC_CMD                 0x02c   /* 8-bit command index */
#define MSC_ARG                 0x030   /* 32-bit argument */
#define MSC_RES                 0x034   /* 16-bit response, sequential */
#define MSC_RXFIFO              0x038   /* 32-bit receive FIFO */
#define MSC_TXFIFO              0x03c   /* 32-bit transmit FIFO */

/* MSC_STAT bits */
#define MSC_STAT_AUTO_CMD_DONE  (1u << 31)
#define MSC_STAT_IS_RESETTING   (1u << 15)
#define MSC_STAT_PRG_DONE       (1u << 13)
#define MSC_STAT_DATA_TRAN_DONE (1u << 12)
#define MSC_STAT_END_CMD_RES    (1u << 11)
#define MSC_STAT_DATA_FIFO_AFULL (1u << 10)
#define MSC_STAT_IS_READWAIT    (1u << 9)
#define MSC_STAT_CLK_EN         (1u << 8)
#define MSC_STAT_DATA_FIFO_FULL (1u << 7)
#define MSC_STAT_DATA_FIFO_EMPTY (1u << 6)
#define MSC_STAT_CRC_RES_ERR    (1u << 5)
#define MSC_STAT_CRC_READ_ERROR (1u << 4)
#define MSC_STAT_TIME_OUT_RES   (1u << 1)
#define MSC_STAT_TIME_OUT_READ  (1u << 0)

/* MSC_IREG / MSC_IMASK bits */
#define MSC_IREG_PRG_DONE       (1u << 1)
#define MSC_IREG_END_CMD_RES    (1u << 2)
#define MSC_IREG_DATA_TRAN_DONE (1u << 0)
#define MSC_IREG_RXFIFO_RD_REQ  (1u << 5)
#define MSC_IREG_TXFIFO_WR_REQ  (1u << 6)

/* RTC registers (JZ4750; offsets from firmware usage) */
#define RTC_RTCCR               0x00    /* bit7 = write-ready            */
#define RTC_RTCSR_SEC           0x34    /* seconds counter (stable read) */
#define RTC_RTCSR_SEC_SET       0x30    /* seconds write port            */

/* ------------------------------------------------------------------ */
/* SD card model 鈥?port of the behaviour-validated python model        */

#include "msc.inc.c"

/* ------------------------------------------------------------------ */
/* INTC 鈥?JZ4750 interrupt controller                                  */
/*   ISR(+0x00): highest-priority pending source number                 */
/*   IMR(+0x04): mask bits (1 = masked/disabled)                        */
/*   IMSR(+0x08): write-1 sets mask                                     */
/*   IMCR(+0x0c): write-1 clears mask (enables)                         */
/*   IPR(+0x10): pending bitmap (read)                                  */

#define TYPE_BOOK2_INTC "book2-intc"
OBJECT_DECLARE_SIMPLE_TYPE(Book2IntcState, BOOK2_INTC)

#define JZ_INTC_NLINES 32

struct Book2IntcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq cpu_irq;

    uint32_t imask;            /* 1 = disabled */
    uint32_t pending;
};

static void book2_intc_update(Book2IntcState *s)
{
    uint32_t active = s->pending & ~s->imask;
    qemu_set_irq(s->cpu_irq, active != 0);
}

static void book2_intc_set_irq(void *opaque, int n, int level)
{
    Book2IntcState *s = opaque;

    if (n < 0 || n >= JZ_INTC_NLINES) {
        return;
    }
    if (level) {
        s->pending |= 1u << n;
    } else {
        s->pending &= ~(1u << n);
    }
    book2_intc_update(s);
}

static uint64_t book2_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2IntcState *s = opaque;

    switch (addr) {
    case 0x00:                            /* ISR: raw source bitmap */
        return s->pending;
    case 0x04:                            /* IMR */
        return s->imask;
    case 0x10:                            /* IPR: unmasked pending */
        return s->pending & ~s->imask;
    default:
        return 0;
    }
}

static void book2_intc_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Book2IntcState *s = opaque;

    switch (addr) {
    case 0x04:                            /* direct IMR write */
        s->imask = (uint32_t)val;
        break;
    case 0x08:                            /* IMSR: set mask bits */
        s->imask |= (uint32_t)val;
        break;
    case 0x0c:                            /* IMCR: clear mask bits */
        s->imask &= ~(uint32_t)val;
        break;
    default:
        break;
    }
    book2_intc_update(s);
}

static const MemoryRegionOps book2_intc_ops = {
    .read = book2_intc_read,
    .write = book2_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void book2_intc_realize(DeviceState *dev, Error **errp)
{
    Book2IntcState *s = BOOK2_INTC(dev);

    /*
     * JZ4750 INTC reset state: IMR = 0 (all lines open) per the manual,
     * but the firmware rewrites the full mask early in its init (the
     * runtime value 0x4097F9FF leaves RTC/SADC/UDC/TCU/GPIOx/LCD open).
     * Model the reset as all-open to match silicon; the firmware's own
     * write then restores its preferred mask.
     */
    s->imask = 0x00000000u;

    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_intc_ops, s,
                          "book2-intc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cpu_irq);
    qdev_init_gpio_in(dev, book2_intc_set_irq, JZ_INTC_NLINES);
}

static void book2_intc_reset(DeviceState *dev)
{
    Book2IntcState *s = BOOK2_INTC(dev);
    s->imask = 0;
    s->pending = 0;
    book2_intc_update(s);
}

static void book2_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_intc_realize;
    device_class_set_legacy_reset(dc, book2_intc_reset);
    dc->user_creatable = false;
}

static const TypeInfo book2_intc_info = {
    .name = TYPE_BOOK2_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2IntcState),
    .class_init = book2_intc_class_init,
};

/* ------------------------------------------------------------------ */
/* RTC 鈥?minimal: write-ready flag + ticking seconds counter           */

#include "board-io.inc.c"

#include "audio.inc.c"

/* ------------------------------------------------------------------ */
/* DMAC 鈥?descriptor-less channel transfers                            */
/*   JZ4750 channels n=0..5: DSAR=+0x20n DTAR=+04 DTCR=+08 DRSR=+0c   */
/*   DCCSR=+0x10 DCMD=+0x14.  The firmware uses NDES mode: it writes  */
/*   DCCSR with bit31 (NDES|EN) and polls bit3 (TT, transfer           */
/*   terminated).  The model performs the copy through the system     */
/*   address space, so a source pointing at an MSC RXFIFO drains the  */
/*   card data stream exactly like the real DMA request line would.   */

#include "dma-timer.inc.c"

#include "display-usb.inc.c"

/* ------------------------------------------------------------------ */
/* Machine                                                              */


#define TYPE_BOOK2_MACHINE MACHINE_TYPE_NAME("book2")

/* Card image paths (single machine instance per process). */
static char *book2_msc0_image;
static char *book2_msc1_image;

static void book2_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
    cpu->env.active_tc.PC = 0x80000000;    /* Boot0 entry, kseg0 */
}

static char *book2_get_msc0_image(Object *obj, Error **errp)
{
    return g_strdup(book2_msc0_image);
}

static void book2_set_msc0_image(Object *obj, const char *value, Error **errp)
{
    g_free(book2_msc0_image);
    book2_msc0_image = g_strdup(value);
}

static char *book2_get_msc1_image(Object *obj, Error **errp)
{
    return g_strdup(book2_msc1_image);
}

static void book2_set_msc1_image(Object *obj, const char *value, Error **errp)
{
    g_free(book2_msc1_image);
    book2_msc1_image = g_strdup(value);
}

/*
 * Mask-ROM equivalent: read the first stage (Boot0, 16 sectors) from
 * LBA 0 of the MSC0 card and place it at the beginning of SDRAM.
 */
static void book2_load_boot0(const char *image, Error **errp)
{
    FILE *f = fopen(image, "rb");
    if (!f) {
        error_setg(errp, "book2: cannot open msc0 image '%s'", image);
        return;
    }
    uint8_t buf[16 * 512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) {
        error_setg(errp, "book2: msc0 image '%s' is empty", image);
        return;
    }
    rom_add_blob_fixed("book2.boot0", buf, n, 0);
}

static DeviceState *book2_make_msc(const char *image, hwaddr base,
                                   qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_BOOK2_MSC);
    if (image) {
        qdev_prop_set_string(dev, "image", image);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return dev;
}

static void book2_init(MachineState *machine)
{
    Clock *cpuclk;
    MIPSCPU *cpu;
    DeviceState *intc;
    MemoryRegion *sysmem = get_system_memory();
    int i;

    if (machine->ram_size < BOOK2_RAM_SIZE) {
        error_report("book2: needs at least 64 MiB of memory "
                     "(use -m 64M)");
        exit(1);
    }

    /* JZ4750 is XBurst MIPS32r1 with MXU1 and no FPU. */
    cpuclk = clock_new(object_get_root(), "book2-cpu-clk");
    clock_set_hz(cpuclk, 12 * 1000 * 1000);
    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, false);
    cpu_mips_irq_init_cpu(cpu);
    qemu_register_reset(book2_cpu_reset, cpu);

    /* RAM: 64 MB at physical 0 (kseg0 0x80000000..). */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /*
     * Generic MMIO shadow: RAM-backed store/readback covering the whole
     * JZ4750 peripheral span. Real devices are overlaid below at higher
     * priority. Anything the firmware configures but the machine does
     * not yet model (CPM, EMC, GPIO, TCU, DMAC, AIC, SADC, ...) keeps
     * the value it wrote, which is the benign behaviour the boot code
     * expects (it configures clocks/SDRAM/pin-mux and only delays).
     */
    {
        MemoryRegion *shadow = g_new(MemoryRegion, 1);
        memory_region_init_ram(shadow, NULL, "book2.mmio-shadow",
                               0x3200000, &error_fatal);
        memory_region_add_subregion_overlap(sysmem, 0x10000000, shadow, 0);
    }

    /* Interrupt controller -> CPU INT2 */
    intc = qdev_new(TYPE_BOOK2_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, JZ_INTC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0, cpu->env.irq[2]);

    /* RTC */
    {
        DeviceState *rtc = qdev_new(TYPE_BOOK2_RTC);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(rtc), 0, JZ_RTC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(rtc), 0, qdev_get_gpio_in(intc, 9));
    }

    {
        DeviceState *gpio = qdev_new(TYPE_BOOK2_GPIO);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, JZ_GPIO_BASE);
        for (i = 0; i < 6; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(gpio), i,
                               qdev_get_gpio_in(intc, 29 - i));
        }
    }
    {
        DeviceState *sadc = qdev_new(TYPE_BOOK2_SADC);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sadc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sadc), 0, JZ_SADC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(sadc), 0, qdev_get_gpio_in(intc, 10));
    }

    /* AIC playback and both DMAC banks. */
    {
        DeviceState *aic = qdev_new(TYPE_BOOK2_AIC);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(aic), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(aic), 0, JZ_AIC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(aic), 0, qdev_get_gpio_in(intc, 8));
        DeviceState *dmac = qdev_new(TYPE_BOOK2_DMAC);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dmac), 0, JZ_DMAC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(dmac), 0, qdev_get_gpio_in(intc, 20));
        sysbus_connect_irq(SYS_BUS_DEVICE(dmac), 1, qdev_get_gpio_in(intc, 15));
        book2_dmac_set_aic(dmac, aic, book2_aic_dma_ready);
    }

    /* Book2 firmware uses TCU1 for all timers; OST -> IRQ21 */
    {
        DeviceState *tcu = qdev_new(TYPE_BOOK2_TCU);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(tcu), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(tcu), 0, JZ_TCU_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(tcu), 0,
                           qdev_get_gpio_in(intc, 23));
        sysbus_connect_irq(SYS_BUS_DEVICE(tcu), 1,
                           qdev_get_gpio_in(intc, 22));
        sysbus_connect_irq(SYS_BUS_DEVICE(tcu), 2,
                           qdev_get_gpio_in(intc, 21));
    }

    /* UART0..3: 16550 with 4-byte register stride; UART3 = console */
    for (i = 0; i < 4; i++) {
        Chardev *chr = (i == 3) ? serial_hd(0) : NULL;
        int irq_line = JZ_IRQ_UART0 - i;   /* 6,5,4,3 */
        serial_mm_init(sysmem, JZ_UART_BASE + i * 0x1000, 2,
                      qdev_get_gpio_in(intc, irq_line),
                      1843200, chr, DEVICE_LITTLE_ENDIAN);
    }

    /* MSC0: system card (Boot0/OS); MSC1: content card (FAT32 A:/B:) */
    if (!book2_msc0_image) {
        error_report("book2: the system card image is required: "
                     "-M book2,msc0-image=<file>");
        exit(1);
    }
    book2_make_msc(book2_msc0_image, JZ_MSC0_BASE,
                   qdev_get_gpio_in(intc, JZ_IRQ_MSC0));
    if (book2_msc1_image) {
        book2_make_msc(book2_msc1_image, JZ_MSC1_BASE,
                       qdev_get_gpio_in(intc, JZ_IRQ_MSC1));
    }

    /* LCD controller -> INTC line 31 */
    {
        DeviceState *lcd = qdev_new(TYPE_BOOK2_LCD);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(lcd), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(lcd), 0, JZ_LCD_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(lcd), 0,
                           qdev_get_gpio_in(intc, 31));
    }


    book2_init_ohci(intc, true);

    /* Load the first stage from the card (mask-ROM equivalent). */
    book2_load_boot0(book2_msc0_image, &error_fatal);
}

static void book2_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "BBK Book 2 learning computer (Ingenic JZ4750)";
    mc->init = book2_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("XBurstR1");
    mc->default_ram_id = "book2.ram";
    mc->default_ram_size = BOOK2_RAM_SIZE;
    mc->no_serial = 0;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    object_class_property_add_str(oc, "msc0-image",
                                  book2_get_msc0_image,
                                  book2_set_msc0_image);
    object_class_property_set_description(oc, "msc0-image",
        "SD card image attached to MSC0 (the system card: "
        "Boot0 at LBA0, OS copies, ExtOS)");
    object_class_property_add_str(oc, "msc1-image",
                                  book2_get_msc1_image,
                                  book2_set_msc1_image);
    object_class_property_set_description(oc, "msc1-image",
        "SD card image attached to MSC1 (the content card with the "
        "FAT32 A:/B: volumes, read by the firmware itself)");
}

static const TypeInfo book2_machine_info = {
    .name = TYPE_BOOK2_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = book2_machine_class_init,
};

static void book2_register_types(void)
{
    type_register_static(&book2_msc_info);
    type_register_static(&book2_intc_info);
    type_register_static(&book2_rtc_info);
    type_register_static(&book2_gpio_info);
    type_register_static(&book2_sadc_info);
    type_register_static(&book2_aic_info);
    type_register_static(&book2_dmac_info);
    type_register_static(&book2_tcu_info);
    type_register_static(&book2_lcd_info);
    type_register_static(&book2_machine_info);
}

type_init(book2_register_types)
