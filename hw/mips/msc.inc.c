/* SD protocol/card storage model. Included after the MSC register defines. */
#define SD_RESP_MAX 20
#define SD_DATA_CAP (4 * MiB)
#define SD_R1_OUT_OF_RANGE (1u << 31)
#define SD_R1_ADDRESS_ERROR (1u << 30)
#define SD_R1_BLOCK_LEN_ERROR (1u << 29)
#define SD_R1_WP_VIOLATION (1u << 26)
#define SD_R1_ILLEGAL_COMMAND (1u << 22)
#define SD_R1_ERROR (1u << 19)
#define SD_R1_READY_FOR_DATA (1u << 8)
#define SD_R1_APP_CMD (1u << 5)
#define SD_STAT_CRC_WRITE_ERROR (1u << 2)

typedef struct SdCardModel {
    FILE *img;
    uint64_t img_size;
    char *img_path;
    bool writable;
    bool clk_active;
    bool xfer_pending;
    uint64_t xfer_off;
    bool cmd_pending;
    uint16_t clkrt;
    uint32_t cmdat;
    uint32_t arg;
    uint16_t blklen, nob, snob, ireg, imask;
    uint8_t cmd, state;
    uint16_t rca;
    uint32_t ocr;
    uint32_t card_error, transfer_error;
    bool app_cmd, data_done, prg_done;
    uint8_t resp[SD_RESP_MAX];
    int resp_len, resp_pos;
    /* Programmable card identity.  A real SD card returns this 128-bit CID
     * from CMD2/CMD10 and the firmware derives the visible machine code from
     * it, so it must be stable across boots and unique per card image. */
    uint32_t cid[4];
    uint8_t *data;
    size_t data_len, data_pos, data_cap;
    bool write_active, write_count_known;
    uint64_t write_off;
    uint32_t write_blocks_remaining;
    uint8_t write_buf[512];
    unsigned write_buf_used;
} SdCardModel;

static bool sd_seek(SdCardModel *s, uint64_t off)
{
    if (off > INT64_MAX) {
        return false;
    }
    clearerr(s->img);
#ifdef _WIN32
    return _fseeki64(s->img, (int64_t)off, SEEK_SET) == 0;
#else
    return fseeko(s->img, (off_t)off, SEEK_SET) == 0;
#endif
}

static void sd_model_init(SdCardModel *s, const char *image, Error **errp)
{
    char *image_property = s->img_path;
    int64_t end;
    memset(s, 0, sizeof(*s));
    s->img_path = image_property;
    s->img = fopen(image, "r+b");
    s->writable = s->img != NULL;
    if (!s->img) {
        s->img = fopen(image, "rb");
    }
    if (!s->img) {
        error_setg(errp, "book2: cannot open SD card image '%s'", image);
        return;
    }
#ifdef _WIN32
    if (_fseeki64(s->img, 0, SEEK_END) != 0 ||
        (end = _ftelli64(s->img)) < 0) {
#else
    if (fseeko(s->img, 0, SEEK_END) != 0 ||
        (end = ftello(s->img)) < 0) {
#endif
        error_setg(errp, "book2: cannot size SD card image '%s'", image);
        fclose(s->img);
        s->img = NULL;
        return;
    }
    s->img_size = end;
    if (!sd_seek(s, 0)) {
        error_setg(errp, "book2: cannot rewind SD card image '%s'", image);
        fclose(s->img);
        s->img = NULL;
        return;
    }
    s->state = 0; /* IDLE; R1 CURRENT_STATE, not SPI idle bit. */
    s->rca = 1;
    s->ocr = 0xC0FF8000;
    s->data_cap = SD_DATA_CAP;
    s->data = g_malloc(s->data_cap);

    /*
     * Card CID: manufacturer "B2" plus a 32-bit serial derived from the card
     * image size and a fixed board salt.  The firmware formats this identity
     * as the device machine code, so it must be reproducible for a given
     * image instead of random per boot (which would change the code on every
     * launch) and non-zero (all-zero leaves the field blank on screen).
     */
    {
        uint32_t serial = (uint32_t)((s->img_size >> 20) ^ 0x42B20000u) |
                          0x00000001u;

        s->cid[0] = 0x02425a32u;              /* CID[127:96]: MID/OID high */
        s->cid[1] = (uint32_t)(s->img_size >> 32) ^ 0x0000b2a7u;
        s->cid[2] = serial;
        s->cid[3] = (uint32_t)(s->img_size & 0xffffffff) >> 8 ^ 0x00005c31u;
    }
}

/* Preserve the validated CSD v2 encoding and real-card template. */
static int sd_build_csd(SdCardModel *s, uint8_t out[16])
{
    uint64_t blocks = s->img_size / 512;
    uint64_t c_size = blocks / 1024;
    static const uint8_t tpl[16] = {
        0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00,
        0xEE, 0xF7, 0x7F, 0xFF, 0x5F, 0x80, 0x0A, 0x40,
    };
    if (c_size) {
        c_size--;
    }
    c_size &= 0x3fffff;
    memcpy(out, tpl, 16);
    out[7] = (out[7] & 0xc0) | ((c_size >> 16) & 0x3f);
    out[8] = (c_size >> 8) & 0xff;
    out[9] = c_size & 0xff;
    return 128;
}

static void sd_r1(SdCardModel *s, uint32_t st, uint8_t header,
                  uint8_t *out, int *out_len)
{
    out[0] = header ? header : (s->cmd & 0x3f);
    out[1] = st >> 24;
    out[2] = st >> 16;
    out[3] = st >> 8;
    out[4] = st;
    out[5] = 0x7f;
    *out_len = 6;
}

static uint32_t sd_card_status(const SdCardModel *s)
{
    uint32_t status = s->card_error | ((uint32_t)s->state << 9);
    if (s->state == 3 || s->state == 4) {
        status |= SD_R1_READY_FOR_DATA;
    }
    return status;
}

static void sd_reply_status(SdCardModel *s)
{
    sd_r1(s, sd_card_status(s), 0, s->resp, &s->resp_len);
}

static bool sd_range(const SdCardModel *s, uint64_t off, uint64_t len)
{
    return off <= s->img_size && len <= s->img_size - off;
}

static void sd_abort_data(SdCardModel *s)
{
    s->xfer_pending = false;
    s->write_active = false;
    s->write_buf_used = 0;
    s->data_len = s->data_pos = 0;
    s->ireg &= ~(MSC_IREG_RXFIFO_RD_REQ | MSC_IREG_TXFIFO_WR_REQ);
}

static void sd_begin_data(SdCardModel *s)
{
    sd_abort_data(s);
    s->data_done = s->prg_done = false;
    s->card_error = s->transfer_error = 0;
    s->ireg &= ~(MSC_IREG_DATA_TRAN_DONE | MSC_IREG_PRG_DONE);
}

static void sd_data_fail(SdCardModel *s, uint32_t card_error,
                         uint32_t controller_error)
{
    sd_abort_data(s);
    s->card_error |= card_error;
    s->transfer_error |= controller_error;
    s->state = 4;
    /* Terminate the host transfer with an error, not a success timeout pulse.
     * Card ERROR/WP and persistent STAT errors distinguish the failure. */
    s->data_done = s->prg_done = true;
    s->ireg |= MSC_IREG_DATA_TRAN_DONE | MSC_IREG_PRG_DONE;
}

static bool sd_load_data(SdCardModel *s, uint64_t off, size_t len)
{
    if (!sd_range(s, off, len) || !sd_seek(s, off)) {
        sd_data_fail(s, SD_R1_ERROR, MSC_STAT_TIME_OUT_READ);
        return false;
    }
    if (len > s->data_cap) {
        s->data = g_realloc(s->data, len);
        s->data_cap = len;
    }
    if (fread(s->data, 1, len, s->img) != len) {
        /* A backing-file short read is a real failure, never fabricated data. */
        sd_data_fail(s, SD_R1_ERROR, MSC_STAT_TIME_OUT_READ);
        return false;
    }
    s->data_len = len;
    s->data_pos = 0;
    return true;
}

static void sd_write_finish(SdCardModel *s)
{
    s->state = 7; /* PRG; synchronous backing store has no artificial busy delay. */
    if (fflush(s->img) != 0) {
        sd_data_fail(s, SD_R1_ERROR, SD_STAT_CRC_WRITE_ERROR);
        return;
    }
    s->write_active = false;
    s->write_buf_used = 0;
    s->state = 4;
    s->data_done = s->prg_done = true;
    s->ireg &= ~MSC_IREG_TXFIFO_WR_REQ;
    s->ireg |= MSC_IREG_DATA_TRAN_DONE | MSC_IREG_PRG_DONE;
}

static void sd_write_txfifo(SdCardModel *s, uint32_t val, unsigned nbytes)
{
    if (!s->write_active || s->cmd_pending || nbytes > 4) {
        return;
    }
    for (unsigned k = 0; k < nbytes && s->write_active; k++) {
        s->write_buf[s->write_buf_used++] = val >> (8 * k);
        if (s->write_buf_used != sizeof(s->write_buf)) {
            continue;
        }
        if (!s->writable) {
            sd_data_fail(s, SD_R1_WP_VIOLATION, SD_STAT_CRC_WRITE_ERROR);
            break;
        }
        if (!sd_range(s, s->write_off, sizeof(s->write_buf))) {
            sd_data_fail(s, SD_R1_OUT_OF_RANGE, SD_STAT_CRC_WRITE_ERROR);
            break;
        }
        if (!sd_seek(s, s->write_off) ||
            fwrite(s->write_buf, 1, sizeof(s->write_buf), s->img) !=
                sizeof(s->write_buf) || fflush(s->img) != 0) {
            sd_data_fail(s, SD_R1_ERROR, SD_STAT_CRC_WRITE_ERROR);
            break;
        }
        s->write_off += sizeof(s->write_buf);
        s->write_buf_used = 0;
        if (s->snob) {
            s->snob--;
        }
        if (s->write_count_known && --s->write_blocks_remaining == 0) {
            sd_write_finish(s);
        }
    }
}

static bool sd_check_data_command(SdCardModel *s, uint64_t off, uint64_t len,
                                  bool write)
{
    if (s->state != 4) {
        s->card_error |= SD_R1_ILLEGAL_COMMAND;
    } else if (write && !s->writable) {
        s->card_error |= SD_R1_WP_VIOLATION;
    } else if (s->blklen && s->blklen != 512) {
        s->card_error |= SD_R1_BLOCK_LEN_ERROR;
    } else if (off & 511) {
        s->card_error |= SD_R1_ADDRESS_ERROR;
    } else if (!sd_range(s, off, len)) {
        s->card_error |= SD_R1_OUT_OF_RANGE;
    } else {
        return true;
    }
    return false;
}

/* Execute the card command; host command/data timers remain below. */
static void sd_execute(SdCardModel *s)
{
    uint8_t cmd = s->cmd & 0x3f;
    uint32_t arg = s->arg;
    bool app = s->app_cmd;
    uint64_t off = (s->ocr & 0x40000000u) ? (uint64_t)arg << 9 : arg;
    s->app_cmd = false;
    s->resp_len = s->resp_pos = 0;
    s->ireg &= ~MSC_IREG_END_CMD_RES;
    s->prg_done = false;
    s->ireg &= ~MSC_IREG_PRG_DONE;

    switch (cmd) {
    case 0:
        sd_begin_data(s);
        s->state = 0;
        sd_reply_status(s);
        break;
    case 8: /* R7 */
        sd_r1(s, arg, 8, s->resp, &s->resp_len);
        /* The firmware's card-probe routine issues CMD8 with DATA_EN set and
         * then drains a 512-byte block from the RX fifo while polling
         * TIME_OUT_READ.  Provide that data phase for the transfer-sized
         * command so the probe observes a completed transfer instead of a
         * permanent read timeout (which made the version page spin). */
        if (s->cmdat & MSC_CMDAT_DATA_EN) {
            sd_begin_data(s);
            memset(s->data, 0, MIN(s->data_cap, 512));
            s->data_len = MIN(s->data_cap, 512);
            s->state = 5;
        }
        break;
    case 55:
        s->app_cmd = true;
        sd_r1(s, sd_card_status(s) | SD_R1_APP_CMD, 0,
              s->resp, &s->resp_len);
        break;
    case 1:
    case 41: /* R3 */
        s->state = 1;
        sd_r1(s, s->ocr, 0x3f, s->resp, &s->resp_len);
        break;
    case 2:
    case 10:
        s->resp[0] = 0x3f;
        for (int i = 0; i < 16; i++) {
            /* CID bytes 1..16 = the 128-bit identity, big-endian. */
            unsigned word = i / 4;
            unsigned shift = 24 - 8 * (i % 4);

            s->resp[1 + i] = (uint8_t)(s->cid[word] >> shift);
        }
        s->resp_len = 17;
        if (cmd == 2) {
            s->state = 2;
        }
        break;
    case 3: /* R6: RCA in high16, lower16 card status */
        s->state = 3;
        sd_r1(s, ((uint32_t)s->rca << 16) | (sd_card_status(s) & 0xffff),
              3, s->resp, &s->resp_len);
        break;
    case 9:
        s->resp[0] = 0x3f;
        sd_build_csd(s, s->resp + 1);
        s->resp_len = 17;
        break;
    case 7:
        s->state = (arg >> 16) == s->rca ? 4 : 3;
        sd_reply_status(s);
        break;
    case 13:
        /* Preserve outstanding RX/TX data and report its actual state. */
        sd_reply_status(s);
        break;
    case 16:
        if (arg != 512) {
            s->card_error |= SD_R1_BLOCK_LEN_ERROR;
        }
        sd_reply_status(s);
        break;
    case 23: /* ACMD23 pre-erase hint: not a change to host NOB. */
        if (!app) {
            s->card_error |= SD_R1_ILLEGAL_COMMAND;
        }
        sd_reply_status(s);
        break;
    case 51: {
        static const uint8_t scr[8] = {
            0x00, 0x40, 0, 0, 0, 0, 0, 0
        };
        sd_begin_data(s);
        sd_reply_status(s);
        memcpy(s->data, scr, sizeof(scr));
        s->data_len = sizeof(scr);
        s->state = 5;
        break;
    }
    case 6:
        if (!app && (s->cmdat & MSC_CMDAT_DATA_EN)) {
            sd_begin_data(s);
            sd_reply_status(s);
            memset(s->data, 0, 64);
            s->data[1] = 1;
            s->data[13] = 0x0f;
            s->data[14] = (arg & 0x0f0000) ? 2 : 1;
            s->data_len = 64;
            s->state = 5;
        } else {
            sd_reply_status(s);
        }
        break;
    case 17:
    case 18: {
        uint32_t nblk = cmd == 17 ? 1 : (s->nob ? s->nob : 1);
        if (!sd_check_data_command(s, off, (uint64_t)nblk * 512, false)) {
            sd_reply_status(s);
            break;
        }
        sd_begin_data(s);
        sd_reply_status(s);
        s->xfer_off = off;
        s->xfer_pending = true;
        s->data_len = (size_t)nblk * 512;
        s->snob = nblk;
        s->state = 5;
        break;
    }
    case 24:
    case 25: {
        uint32_t nblk = cmd == 24 ? 1 : s->nob;
        uint64_t bytes = (uint64_t)(nblk ? nblk : 1) * 512;
        if (!sd_check_data_command(s, off, bytes, true)) {
            sd_reply_status(s);
            break;
        }
        sd_begin_data(s);
        sd_reply_status(s); /* Accepted while in TRAN; data phase enters RCV. */
        s->write_active = true;
        s->write_count_known = nblk != 0;
        s->write_blocks_remaining = nblk;
        s->write_off = off;
        s->snob = nblk;
        s->state = 6;
        break;
    }
    case 60:
        /*
         * Card-identity query used by the firmware's machine-code screen.
         * The firmware drives this as command 0x3c and reads a 17-frame
         * response (leading 0x3f plus the card's 128-bit identifier) to
         * format the displayed machine code.  Serve the same CID the card
         * reports for CMD2/CMD10, so the code is stable per card image.
         */
        s->resp[0] = 0x3f;
        for (int i = 0; i < 16; i++) {
            unsigned word = i / 4;
            unsigned shift = 24 - 8 * (i % 4);

            s->resp[1 + i] = (uint8_t)(s->cid[word] >> shift);
        }
        s->resp_len = 17;
        break;
    case 12:
        if (s->write_active) {
            /* Only full blocks have been committed. Abort a partial block. */
            sd_write_finish(s);
        } else {
            /* STOP_TRANSMISSION.  If no data transfer is outstanding there is
             * nothing to terminate, so this must not re-assert the transfer
             * completion flag.  Doing so made the flag permanently pending:
             * the guest cleared IREG with the W1C write and this branch set it
             * straight back, leaving the MSC interrupt line asserted and the
             * CPU stuck entering the handler forever (the guest appeared to
             * hang a second or two into any page that played audio, because
             * that path issues CMD12 repeatedly). */
            if (s->data_len || s->data_done || s->xfer_pending) {
                sd_abort_data(s);
                s->state = 4;
                s->data_done = true;
                s->ireg |= MSC_IREG_DATA_TRAN_DONE;
            }
        }
        sd_reply_status(s);
        break;
    default:
        s->card_error |= SD_R1_ILLEGAL_COMMAND;
        sd_reply_status(s);
        break;
    }
    s->cmd_pending = true;
    s->clk_active = true;
}

static uint32_t sd_stat(SdCardModel *s)
{
    uint32_t st = s->transfer_error;
    if (!s->cmd_pending) {
        st |= MSC_STAT_END_CMD_RES;
    }
    if (s->prg_done) {
        st |= MSC_STAT_PRG_DONE;
    }
    if (s->data_done) {
        st |= MSC_STAT_DATA_TRAN_DONE;
    }
    if (s->clk_active) {
        st |= MSC_STAT_CLK_EN;
    }
    if (s->write_active || s->xfer_pending || s->data_pos >= s->data_len) {
        st |= MSC_STAT_DATA_FIFO_EMPTY;
    } else {
        st |= MSC_STAT_DATA_FIFO_FULL | MSC_STAT_DATA_FIFO_AFULL;
    }
    return st; /* Observational: never acknowledge a status/error by reading. */
}

static uint16_t sd_read_res(SdCardModel *s, unsigned size)
{
    int i = s->resp_pos++ * 2;
    if (s->resp_len == 6 && i == 4) {
        /* Last 16-bit word of a 48-bit R1/R6/R7 response.  Two independent
         * consumers constrain it, and both are satisfied by carrying the two
         * remaining status bytes:
         *  - the factory kernel (jz4750_mmc.c, format R1) takes only the low
         *    byte here as status[7:0];
         *  - the firmware's own card routine stores the high byte in its
         *    status buffer and validates it as READY_FOR_DATA plus the
         *    CURRENT_STATE nibble (state TRAN = 4).  Returning only a CRC
         *    byte there leaves that check permanently false and the routine
         *    retries forever, which is what froze the version page. */
        return (uint16_t)((s->resp[3] << 8) | s->resp[4]);
    }
    if (i + 1 < s->resp_len) {
        return ((uint16_t)s->resp[i] << 8) | s->resp[i + 1];
    }
    if (i < s->resp_len) {
        return (uint16_t)s->resp[i] << 8;
    }
    return 0xffff;
}

static uint32_t sd_read_rxfifo(SdCardModel *s, unsigned nbytes)
{
    uint32_t val = 0;
    if (s->xfer_pending || s->write_active || nbytes > 4) {
        return val;
    }
    if (getenv("BOOK2_MSC_TRACE")) {
        fprintf(stderr, "msc-rxfifo read n=%u pos=%zu len=%zu\n",
                nbytes, s->data_pos, s->data_len);
    }
    for (unsigned k = 0; k < nbytes; k++) {
        if (s->data_pos < s->data_len) {
            val |= (uint32_t)s->data[s->data_pos++] << (8 * k);
        }
    }
    if (s->data_len && s->data_pos >= s->data_len) {
        s->data_done = s->prg_done = true;
        s->state = 4;
        s->snob = 0;
        s->ireg |= MSC_IREG_DATA_TRAN_DONE | MSC_IREG_PRG_DONE;
        s->ireg &= ~MSC_IREG_RXFIFO_RD_REQ;
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* MSC host controller device                                          */

#define TYPE_BOOK2_MSC "book2-msc"
OBJECT_DECLARE_SIMPLE_TYPE(Book2MscState, BOOK2_MSC)

struct Book2MscState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    SdCardModel card;
    bool card_attached;
    uint16_t strpcl_store;

    /* Data-read latency: a real card streams CMD17/18 data over the
     * bus (12.5 MB/s class) — the transfer is NOT instantaneous.  The
     * firmware's driver polls IREG.RXFIFO_RD_REQ before draining the
     * FIFO, so we delay only the data-ready assertion with a virtual
     * clock timer; STAT meanwhile reports a live transfer (CLK_EN,
     * FIFO full pending).  This models the real timing window in which
     * the concurrent A-disk mount (0x80007864) completes before the
     * cross-load finishes — on the real device the mount (a few
     * sectors) always finishes long before the 6 MB OS load. */
    QEMUTimer *xfer_timer;

    /* Command completion latency: a real card needs ~100 µs to answer
     * any command (clock runs, END_CMD_RES rises late).  The firmware
     * polls STAT bit11 after START_OP (0x8002DD30) and STAT bit8 going
     * low after the command (0x8002D79C).  More importantly for the
     * boot race: the cross-load's out-of-lock inter-block commands
     * (CMD12/13/16/55/23) each keep the bus busy for this window —
     * this is the gap in which the A-disk mount task (0x80007864)
     * wins the MSC semaphore and completes before the load finishes. */
    QEMUTimer *cmd_timer;

    /* statistics */
    uint64_t ncmds;
};

static void book2_msc_update_irq(Book2MscState *s)
{
    SdCardModel *c = &s->card;
    /* FIFO request lines are hardware-driven levels.  Recompute from the
     * current FIFO state instead of clearing and re-raising on every call:
     * the old "clear then maybe set" sequence briefly dropped the line, so a
     * guest that happened to sample in that window could miss the request. */
    c->ireg &= ~(MSC_IREG_RXFIFO_RD_REQ | MSC_IREG_TXFIFO_WR_REQ);
    if (!c->cmd_pending) {
        if (c->write_active) {
            c->ireg |= MSC_IREG_TXFIFO_WR_REQ;
        } else if (!c->xfer_pending && c->data_pos < c->data_len) {
            c->ireg |= MSC_IREG_RXFIFO_RD_REQ;
        }
    }
    uint16_t active = c->ireg & ~c->imask;
    qemu_set_irq(s->irq, active ? 1 : 0);
}

/* Real-card data latency: the CMD17/18 stream completes after a
 * realistic bus time (12.5 MB/s class).  Called on the virtual clock. */
static void book2_msc_xfer_done(void *opaque)
{
    Book2MscState *s = opaque;
    SdCardModel *c = &s->card;

    if (!c->xfer_pending) {
        return;
    }
    c->xfer_pending = false;
    if (sd_load_data(c, c->xfer_off, c->data_len)) {
        c->ireg |= MSC_IREG_RXFIFO_RD_REQ;
    }
    book2_msc_update_irq(s);
}

static void book2_msc_xfer_cancel(Book2MscState *s)
{
    if (s->card.xfer_pending) {
        timer_del(s->xfer_timer);
        s->card.xfer_pending = false;
        s->card.data_len = 0;
        s->card.data_pos = 0;
    }
}

/* Real-card command latency: the response completes ~100 µs after
 * START_OP (clock running meanwhile). */
static void book2_msc_cmd_done(void *opaque)
{
    Book2MscState *s = opaque;

    if (!s->card.cmd_pending) {
        return;
    }
    s->card.cmd_pending = false;
    s->card.ireg |= MSC_IREG_END_CMD_RES;
    if (!s->card.write_active && !s->card.xfer_pending &&
        s->card.data_pos >= s->card.data_len) {
        s->card.prg_done = true;
        s->card.ireg |= MSC_IREG_PRG_DONE;
        if (!s->card.data_len) {
            s->card.data_done = true;
            s->card.ireg |= MSC_IREG_DATA_TRAN_DONE;
        }
    }
    book2_msc_update_irq(s);
}

static uint64_t book2_msc_read(void *opaque, hwaddr addr, unsigned size)
{
    Book2MscState *s = opaque;

    if (!s->card_attached) {
        return 0;
    }

    switch (addr) {
    case MSC_STAT: {
        uint32_t st = sd_stat(&s->card);
        trace_book2_msc_stat(st);
        return st;
    }
    case MSC_RES: {
        uint16_t res = sd_read_res(&s->card, size);
        trace_book2_msc_res(res, s->card.resp_pos, s->card.resp_len);
        return res;
    }
    case MSC_RXFIFO: {
        uint32_t value = sd_read_rxfifo(&s->card, size);
        book2_msc_update_irq(s);
        return value;
    }
    case MSC_IREG:
        return s->card.ireg;
    case MSC_IMASK:
        return s->card.imask;
    case MSC_CMD:
        return s->card.cmd;
    case MSC_ARG:
        return s->card.arg;
    case MSC_CMDAT:
        return s->card.cmdat;
    case MSC_BLKLEN:
        return s->card.blklen;
    case MSC_NOB:
        return s->card.nob;
    case MSC_SNOB:
        return s->card.snob;
    case MSC_CLKRT:
        return s->card.clkrt;
    default:
        return 0;
    }
}

static void book2_msc_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Book2MscState *s = opaque;

    if (!s->card_attached) {
        return;
    }

    switch (addr) {
    case MSC_STRPCL: {
        uint16_t v = (uint16_t)val;
        trace_book2_msc_strpcl(v);
        if (v & 0x08) {                      /* RESET */
            book2_msc_xfer_cancel(s);
            if (s->card.cmd_pending) {
                timer_del(s->cmd_timer);
                s->card.cmd_pending = false;
            }
            sd_abort_data(&s->card);
            s->card.transfer_error = 0;
            s->card.data_done = s->card.prg_done = false;
            if (s->card.state == 5 || s->card.state == 6) {
                s->card.state = 4;
            }
            s->card.resp_len = 0;
            s->card.resp_pos = 0;
            s->card.data_len = 0;
            s->card.data_pos = 0;
            s->card.ireg = 0;
            s->card.clk_active = false;
        } else if (v & 0x04) {               /* START_OP */
            /* A new command begins a new data phase and acknowledges
             * the previous transfer's end-of-data indication. */
            CPUState *cs = current_cpu;
            uint32_t cmdpc = 0;
            if (cs) {
                cmdpc = (uint32_t)MIPS_CPU(cs)->env.active_tc.PC;
            }
            /*
             * Starting a command retires the previous command's completion
             * status.  The firmware drains these by writing IREG with
             * write-1-to-clear and does not program IMASK, so any completion
             * flag that survives the clear keeps the MSC interrupt line
             * asserted and the guest re-enters the handler forever (observed
             * as the machine appearing to hang).  Clearing here matches the
             * hardware: a fresh START_OP supersedes the previous response.
             */
            s->card.ireg &= ~(MSC_IREG_END_CMD_RES | MSC_IREG_PRG_DONE |
                              MSC_IREG_DATA_TRAN_DONE);
            trace_book2_msc_cmd(s->card.cmd, s->card.arg, s->card.cmdat,
                                s->card.nob, cmdpc);
            sd_execute(&s->card);
            trace_book2_msc_ireg(s->card.ireg);
            if (s->card.xfer_pending) {
                /* schedule the data stream at a real-card class-4 pace
                 * (~1.25 MB/s ≈ 50 ms per 64 KB group).  Run QEMU with
                 * -icount so the virtual clock advances with executed
                 * instructions: the firmware's instruction-count
                 * timeouts and this delay then share the same time
                 * base, and the concurrent A-disk mount (a few
                 * sectors) completes during the first load groups —
                 * the real device's ordering. */
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                int64_t us = (int64_t)s->card.data_len * 3 / 4;
                if (us < 50) {
                    us = 50;
                }
                timer_mod(s->xfer_timer, now + us * 1000);
            }
            if (s->card.cmd_pending) {
                /* response latency: ~100 µs of card think time; the
                 * completion flags (END_CMD_RES etc.) rise when it
                 * fires — see book2_msc_cmd_done */
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                timer_mod(s->cmd_timer, now + 100000);
            }
            s->ncmds++;
        }
        /* CLOCK_CONTROL is independent of FIFO consumption:
         * 01 stops the serial clock, 10 starts it (factory regs.h).
         * The guest explicitly stops it before each command. */
        switch (v & 0x03) {
        case 1: s->card.clk_active = false; break;
        case 2: s->card.clk_active = true; break;
        default: break;
        }
        s->strpcl_store = 0;                /* write-to-trigger: self-clears */
        break;
    }
    case MSC_TXFIFO:
        sd_write_txfifo(&s->card, (uint32_t)val, size);
        break;
    case MSC_CMDAT:
        s->card.cmdat = (uint32_t)val;
        break;
    case MSC_CMD:
        s->card.cmd = (uint8_t)(val & 0x3f);
        break;
    case MSC_ARG:
        s->card.arg = (uint32_t)val;
        break;
    case MSC_BLKLEN:
        s->card.blklen = (uint16_t)val;
        break;
    case MSC_NOB:
        s->card.nob = (uint16_t)val;
        break;
    case MSC_CLKRT:
        s->card.clkrt = (uint16_t)val;
        break;
    case MSC_IMASK:
        s->card.imask = (uint16_t)val;
        break;
    case MSC_IREG:                          /* write-1-to-clear */
        if (getenv("BOOK2_MSC_TRACE")) {
            fprintf(stderr, "msc-ireg w1c 0x%04x (before 0x%04x)\n",
                    (unsigned)(uint16_t)val, s->card.ireg);
        }
        s->card.ireg &= ~(uint16_t)val;
        break;
    default:
        break;
    }
    book2_msc_update_irq(s);
}

static const MemoryRegionOps book2_msc_ops = {
    .read = book2_msc_read,
    .write = book2_msc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void book2_msc_realize(DeviceState *dev, Error **errp)
{
    Book2MscState *s = BOOK2_MSC(dev);

    s->xfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  book2_msc_xfer_done, s);
    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 book2_msc_cmd_done, s);

    if (s->card.img_path && s->card.img_path[0]) {
        Error *local = NULL;
        sd_model_init(&s->card, s->card.img_path, &local);
        if (local) {
            error_propagate(errp, local);
            return;
        }
        s->card_attached = true;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &book2_msc_ops, s,
                          "book2-msc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property book2_msc_properties[] = {
    DEFINE_PROP_STRING("image", Book2MscState, card.img_path),
};

static void book2_msc_reset(DeviceState *dev)
{
    Book2MscState *s = BOOK2_MSC(dev);
    SdCardModel *c = &s->card;
    if (s->xfer_timer) {
        timer_del(s->xfer_timer);
    }
    if (s->cmd_timer) {
        timer_del(s->cmd_timer);
    }
    sd_abort_data(c);
    c->cmd_pending = c->clk_active = false;
    c->resp_len = c->resp_pos = 0;
    c->clkrt = c->blklen = c->nob = c->snob = 0;
    c->cmdat = c->arg = 0;
    c->ireg = 0;
    c->imask = 0xffff;
    c->cmd = c->state = 0;
    c->rca = 1;
    c->app_cmd = false;
    c->card_error = c->transfer_error = 0;
    c->data_done = c->prg_done = false;
    c->write_off = c->write_blocks_remaining = 0;
    c->write_count_known = false;
    s->strpcl_store = 0;
    /* Complete blocks were already flushed; keep image handle/protection. */
    qemu_set_irq(s->irq, 0);
}

static void book2_msc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = book2_msc_realize;
    device_class_set_legacy_reset(dc, book2_msc_reset);
    device_class_set_props(dc, book2_msc_properties);
    /* the card image is fixed at machine startup */
    dc->user_creatable = false;
}

static const TypeInfo book2_msc_info = {
    .name = TYPE_BOOK2_MSC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Book2MscState),
    .class_init = book2_msc_class_init,
};

