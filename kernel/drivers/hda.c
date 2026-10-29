#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* Intel High Definition Audio: the sound controller in nearly every PC since
 * 2004, and what QEMU emulates with -device intel-hda + hda-duplex/micro.
 *
 * The controller talks to codecs over a serial link. We send codec verbs
 * through the CORB/RIRB rings (the immediate command registers are the
 * fallback), walk the codec's widget graph to find a DAC -> output pin path
 * and an input pin -> ADC path, and run one output and one input DMA stream.
 *
 * The hardware always runs at 48 kHz, 16-bit, stereo: every codec supports
 * it. Other rates and mono are converted here, so programs (the speech code
 * wants 16 kHz mono) need not care. Programs see the OSS interface: /dev/dsp
 * for PCM and /dev/mixer for volume.
 *
 * Each stream's DMA buffer is a ring the hardware loops over forever. Output
 * positions are tracked as two byte counters: app_pos (written by programs)
 * and hw_pos (consumed by the hardware, from LPIB). Whatever the hardware has
 * consumed is zeroed straight away, so if a program falls behind, the
 * hardware plays silence rather than the previous lap again. Positions are
 * updated by the buffer-completion interrupt (MSI) and by a service thread
 * that polls LPIB while a stream runs, in case interrupts never arrive. */

/* ---- controller registers ---- */
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0e
#define INTCTL    0x20
#define INTSTS    0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4a
#define CORBCTL   0x4c
#define CORBSIZE  0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5a
#define RIRBCTL   0x5c
#define RIRBSTS   0x5d
#define RIRBSIZE  0x5e
#define ICOI      0x60
#define ICII      0x64
#define ICIS      0x68

/* stream descriptor registers, relative to 0x80 + 0x20 * index */
#define SD_CTL   0x00 /* bytes 0-2; byte 2 holds the stream tag */
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0c
#define SD_FIFOS 0x10
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1c

#define SD_CTL_SRST 0x01
#define SD_CTL_RUN  0x02
#define SD_CTL_IOCE 0x04
#define SD_STS_MASK 0x1c /* BCIS, FIFOE, DESE: write 1 to clear */

/* ---- codec verbs ---- */
#define VERB_GET_PARAM      0xf00
#define VERB_GET_CONN_LIST  0xf02
#define VERB_SET_CONN_SEL   0x701
#define VERB_SET_POWER      0x705
#define VERB_SET_STREAM     0x706
#define VERB_SET_PIN_CTL    0x707
#define VERB_SET_EAPD       0x70c
#define VERB_GET_CONFIG     0xf1c
#define VERB4_SET_FORMAT    0x2 /* 4-bit verbs carry a 16-bit payload */
#define VERB4_SET_AMP       0x3

#define PARAM_VENDOR     0x00
#define PARAM_NODE_COUNT 0x04
#define PARAM_FG_TYPE    0x05
#define PARAM_WCAPS      0x09
#define PARAM_PINCAPS    0x0c
#define PARAM_AMP_IN     0x0d
#define PARAM_CONN_LEN   0x0e
#define PARAM_AMP_OUT    0x12

/* widget types (audio widget capabilities bits 23:20) */
#define WT_DAC      0
#define WT_ADC      1
#define WT_MIXER    2
#define WT_SELECTOR 3
#define WT_PIN      4

#define WCAP_IN_AMP   (1u << 1)
#define WCAP_OUT_AMP  (1u << 2)
#define WCAP_AMP_OVRD (1u << 3)
#define PINCAP_HP     (1u << 3)
#define PINCAP_OUT    (1u << 4)
#define PINCAP_IN     (1u << 5)
#define PINCAP_VREF50 (1u << 9)
#define PINCAP_VREF80 (1u << 12)
#define PINCAP_EAPD   (1u << 16)

/* pin default configuration: device type (bits 23:20) */
#define DEV_LINE_OUT 0x0
#define DEV_SPEAKER  0x1
#define DEV_HP_OUT   0x2
#define DEV_LINE_IN  0x8
#define DEV_MIC_IN   0xa

/* ---- stream format and buffering ---- */
#define HW_RATE       48000
#define FRAME         4 /* bytes per hardware frame: 16-bit stereo */
#define BYTES_PER_SEC (HW_RATE * FRAME)
#define FMT_48K_S16_2 0x0011 /* base 48 kHz, x1 /1, 16 bits, 2 channels */
#define RING          65536 /* 341 ms of audio per direction */
#define PERIOD        4096  /* one BDL entry, 21 ms: interrupt granularity */
#define NBDL          (RING / PERIOD)
#define OUT_TAG       1
#define IN_TAG        2
#define VEC_HDA       0x2c

/* A stream whose position has not moved for this long is treated as dead:
 * there is no backend behind the codec (e.g. QEMU's wav audiodev has no
 * capture side) or the hardware stopped. Its clock is then simulated from
 * the wall clock with silence, so programs never hang on it. */
#define STALL_NS (400ULL * 1000000)
/* Output only goes dead when the hardware is broken (every backend consumes),
 * and stays dead until the stream next stops: be slow to give up on it. */
#define OUT_STALL_NS (1000ULL * 1000000)
/* After an underrun, new data goes this far ahead of the hardware so the
 * DMA engine (which prefetches into its FIFO) does not miss its start. */
#define UNDERRUN_GAP 1024
/* An output stream with nothing queued for this long is stopped. Longer than
 * any codec-side buffering, so stopping never cuts sound off. */
#define IDLE_STOP_NS (300ULL * 1000000)

struct bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags; /* bit 0: interrupt on completion */
} __attribute__((packed));

struct stream {
    bool present;
    bool output;
    int index; /* stream descriptor number */
    uint8_t tag;
    uint32_t sd;   /* register offset of the descriptor */
    uint8_t *ring; /* RING bytes of DMA memory */
    uint64_t ring_phys;
    struct bdl_entry *bdl;
    uint64_t bdl_phys;
    bool running;
    bool dead;
    uint32_t last_lpib;
    uint64_t hw_pos;  /* bytes the hardware has played / captured */
    uint64_t app_pos; /* bytes programs have written / read */
    uint64_t progress_ns; /* last time hw_pos moved */
    uint64_t dead_ns, dead_pos; /* simulated clock origin */
    uint64_t idle_ns; /* output: when the hardware ran out of data, or 0 */
    uint32_t guard; /* capture: bytes behind LPIB that may still be in the FIFO */
};

#define MAX_CONN  32
#define MAX_DEPTH 6

struct widget {
    uint16_t nid;
    uint8_t type;
    uint8_t nconn;
    uint32_t caps, pincaps, config, amp_in, amp_out;
    uint16_t conn[MAX_CONN];
};

struct hda_path {
    int len;
    uint16_t nid[MAX_DEPTH];
    uint8_t idx[MAX_DEPTH]; /* connection index on nid[i] that leads to nid[i+1] */
};

struct dsp_file;

static struct {
    volatile uint8_t *mmio;
    struct pci_dev *pci;
    bool irq;
    /* command interface */
    bool use_corb;
    uint32_t *corb;
    uint32_t *rirb;
    uint32_t corb_entries, rirb_entries;
    uint16_t corb_wp, rirb_rp;
    int cmd_timeouts;
    spinlock_t cmd_lock;
    /* codec */
    uint8_t cad;
    uint16_t afg;
    uint32_t afg_amp_in, afg_amp_out;
    struct widget *w;
    int nw;
    uint16_t first_nid;
    /* streams */
    struct stream out, in;
    struct dsp_file *writer, *reader;
    spinlock_t lock;
    struct waitq wq;     /* programs wait here for space / data */
    struct waitq kick;   /* the service thread waits here while idle */
    /* mixer: OSS encoding, left | right << 8, each 0-100 */
    uint32_t vol_master, vol_pcm, vol_mic;
} hda;

/* ---- register access ---- */

static inline uint8_t rd8(uint32_t r) { return *(volatile uint8_t *)(hda.mmio + r); }
static inline uint16_t rd16(uint32_t r) { return *(volatile uint16_t *)(hda.mmio + r); }
static inline uint32_t rd32(uint32_t r) { return *(volatile uint32_t *)(hda.mmio + r); }
static inline void wr8(uint32_t r, uint8_t v) { *(volatile uint8_t *)(hda.mmio + r) = v; }
static inline void wr16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(hda.mmio + r) = v; }
static inline void wr32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(hda.mmio + r) = v; }

/* Time since `since`, or 0 if `since` is later: callers read the clock
 * before taking the lock, so another CPU may have stored a newer time. */
static inline uint64_t elapsed(uint64_t now, uint64_t since) { return now > since ? now - since : 0; }

static void udelay(uint64_t us) {
    uint64_t end = time_monotonic_ns() + us * 1000;
    while (time_monotonic_ns() < end)
        cpu_relax();
}

/* Polls until (reg & mask) == want or `us` microseconds pass. */
static bool wait8(uint32_t reg, uint8_t mask, uint8_t want, uint64_t us) {
    uint64_t end = time_monotonic_ns() + us * 1000;
    while ((rd8(reg) & mask) != want)
        if (time_monotonic_ns() > end)
            return false;
    return true;
}

static void *dma_alloc(size_t bytes, uint64_t *phys) {
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t p = pmm_alloc(pages);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    *phys = p;
    return v;
}

/* ---- codec commands ---- */

static bool corb_cmd(uint32_t cmd, uint32_t *resp) {
    hda.corb_wp = (uint16_t)((hda.corb_wp + 1) % hda.corb_entries);
    hda.corb[hda.corb_wp] = cmd;
    barrier();
    wr16(CORBWP, hda.corb_wp);
    uint64_t end = time_monotonic_ns() + 20 * 1000000ULL;
    for (;;) {
        uint16_t wp = rd16(RIRBWP) & 0xff;
        while (hda.rirb_rp != wp) {
            hda.rirb_rp = (uint16_t)((hda.rirb_rp + 1) % hda.rirb_entries);
            uint32_t r = hda.rirb[hda.rirb_rp * 2], ex = hda.rirb[hda.rirb_rp * 2 + 1];
            /* Clearing the response interrupt flag also lets controllers
             * (QEMU among them) that pause after RINTCNT responses go on. */
            wr8(RIRBSTS, 0x05);
            if (ex & 0x10)
                continue; /* unsolicited (jack events): not enabled, ignore */
            *resp = r;
            return true;
        }
        if (time_monotonic_ns() > end)
            return false;
        cpu_relax();
    }
}

static bool immediate_cmd(uint32_t cmd, uint32_t *resp) {
    uint64_t end = time_monotonic_ns() + 20 * 1000000ULL;
    while (rd16(ICIS) & 1)
        if (time_monotonic_ns() > end)
            return false;
    wr32(ICOI, cmd);
    wr16(ICIS, 0x3); /* clear "result valid", start */
    while ((rd16(ICIS) & 3) != 2)
        if (time_monotonic_ns() > end)
            return false;
    *resp = rd32(ICII);
    return true;
}

/* `verb` is either a 12-bit verb shifted left by 8 with an 8-bit payload,
 * or a 4-bit verb shifted left by 16 with a 16-bit payload. */
static uint32_t codec_cmd(uint16_t nid, uint32_t verb) {
    uint32_t cmd = ((uint32_t)hda.cad << 28) | ((uint32_t)(nid & 0x7f) << 20) | (verb & 0xfffff);
    uint32_t resp = 0;
    uint64_t f = spin_lock_irqsave(&hda.cmd_lock);
    bool ok = hda.use_corb ? corb_cmd(cmd, &resp) : immediate_cmd(cmd, &resp);
    spin_unlock_irqrestore(&hda.cmd_lock, f);
    if (!ok) {
        hda.cmd_timeouts++;
        return 0;
    }
    return resp;
}

static uint32_t verb12(uint16_t nid, uint32_t verb, uint8_t payload) {
    return codec_cmd(nid, (verb << 8) | payload);
}
static uint32_t param(uint16_t nid, uint8_t p) { return verb12(nid, VERB_GET_PARAM, p); }

static bool setup_corb(void) {
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wait8(CORBCTL, 0x02, 0, 1000);
    wait8(RIRBCTL, 0x02, 0, 1000);
    uint64_t phys;
    void *mem = hda.corb ? NULL : dma_alloc(PAGE_SIZE, &phys); /* CORB 1 KiB + RIRB 2 KiB */
    if (!mem)
        return false;
    hda.corb = mem;
    hda.rirb = (uint32_t *)((uint8_t *)mem + 2048);

    /* Pick the largest ring each side supports (capability bits 7:4). */
    uint8_t cs = rd8(CORBSIZE), rs = rd8(RIRBSIZE);
    uint8_t csel = (cs & 0x40) ? 2 : (cs & 0x20) ? 1 : 0;
    uint8_t rsel = (rs & 0x40) ? 2 : (rs & 0x20) ? 1 : 0;
    static const uint32_t entries[] = {2, 16, 256};
    hda.corb_entries = entries[csel];
    hda.rirb_entries = entries[rsel];
    wr8(CORBSIZE, (uint8_t)((cs & 0xfc) | csel));
    wr8(RIRBSIZE, (uint8_t)((rs & 0xfc) | rsel));

    wr32(CORBLBASE, (uint32_t)phys);
    wr32(CORBUBASE, (uint32_t)(phys >> 32));
    wr32(RIRBLBASE, (uint32_t)(phys + 2048));
    wr32(RIRBUBASE, (uint32_t)((phys + 2048) >> 32));

    /* Reset the read pointer. Some controllers latch bit 15 until it is
     * cleared again, others never show it: wait briefly either way. */
    wr16(CORBRP, 0x8000);
    for (int i = 0; i < 1000 && !(rd16(CORBRP) & 0x8000); i++)
        udelay(1);
    wr16(CORBRP, 0);
    for (int i = 0; i < 1000 && (rd16(CORBRP) & 0x8000); i++)
        udelay(1);
    wr16(CORBWP, 0);
    hda.corb_wp = 0;

    wr16(RIRBWP, 0x8000); /* reset the write pointer */
    hda.rirb_rp = 0;
    wr16(RINTCNT, 1);
    /* Response interrupts stay off at the controller (INTCTL.CIE clear);
     * RINTCTL only makes the status flag work, which corb_cmd clears. */
    wr8(RIRBCTL, 0x03);
    wr8(CORBCTL, 0x02);
    return wait8(CORBCTL, 0x02, 0x02, 1000);
}

/* ---- controller reset ---- */

static bool controller_reset(void) {
    /* Stop every stream and the command rings before resetting. */
    uint16_t gcap = rd16(GCAP);
    int nstreams = ((gcap >> 12) & 0xf) + ((gcap >> 8) & 0xf) + ((gcap >> 3) & 0x1f);
    for (int i = 0; i < nstreams; i++)
        wr8(0x80 + 0x20 * (uint32_t)i + SD_CTL, 0);
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wr32(INTCTL, 0);

    wr32(GCTL, rd32(GCTL) & ~1u);
    uint64_t end = time_monotonic_ns() + 100 * 1000000ULL;
    while (rd32(GCTL) & 1)
        if (time_monotonic_ns() > end)
            return false;
    udelay(150); /* hold reset at least 100 us */
    wr32(GCTL, rd32(GCTL) | 1);
    end = time_monotonic_ns() + 100 * 1000000ULL;
    while (!(rd32(GCTL) & 1))
        if (time_monotonic_ns() > end)
            return false;
    /* Codecs announce themselves in STATESTS within 521 us of reset. */
    end = time_monotonic_ns() + 100 * 1000000ULL;
    udelay(1000);
    while (!(rd16(STATESTS) & 0x7fff) && time_monotonic_ns() < end)
        udelay(100);
    return true;
}

/* ---- widget graph ---- */

static struct widget *widget(uint16_t nid) {
    if (nid < hda.first_nid || nid >= hda.first_nid + hda.nw)
        return NULL;
    return &hda.w[nid - hda.first_nid];
}

static void read_connections(struct widget *w) {
    uint32_t len = param(w->nid, PARAM_CONN_LEN);
    bool longform = len & 0x80;
    int n = (int)(len & 0x7f);
    int per = longform ? 2 : 4, bits = longform ? 16 : 8;
    uint16_t prev = 0;
    for (int i = 0; i < n && w->nconn < MAX_CONN; i += per) {
        uint32_t r = verb12(w->nid, VERB_GET_CONN_LIST, (uint8_t)i);
        for (int k = 0; k < per && i + k < n && w->nconn < MAX_CONN; k++) {
            uint16_t e = (uint16_t)((r >> (k * bits)) & ((1u << bits) - 1));
            bool range = e & (1u << (bits - 1));
            e &= (uint16_t)((1u << (bits - 1)) - 1);
            if (range && prev) { /* "prev..e" written as a range */
                for (uint16_t x = (uint16_t)(prev + 1); x <= e && w->nconn < MAX_CONN; x++)
                    w->conn[w->nconn++] = x;
            } else {
                w->conn[w->nconn++] = e;
            }
            prev = e;
        }
    }
}

static uint8_t pin_device(const struct widget *w) { return (w->config >> 20) & 0xf; }
static bool pin_connected(const struct widget *w) { return ((w->config >> 30) & 3) != 1; }

static bool match_dac(const struct widget *w, uint16_t want) {
    return w->type == WT_DAC && (!want || w->nid == want);
}

static bool match_in_pin(const struct widget *w, uint16_t want) {
    if (w->type != WT_PIN || !(w->pincaps & PINCAP_IN) || !pin_connected(w))
        return false;
    uint8_t dev = pin_device(w);
    if (want != 0xff)
        return dev == want;
    return dev == DEV_MIC_IN || dev == DEV_LINE_IN || dev == 0x9 /* aux */ || dev == 0xb /* other */;
}

/* Depth-first search from `nid` through connection lists (which name each
 * widget's inputs) for a widget that satisfies `match`, passing only through
 * mixers and selectors. Fills `p` with the route. */
static bool find_path(struct hda_path *p, int depth, uint16_t nid, bool (*match)(const struct widget *, uint16_t),
                      uint16_t arg) {
    struct widget *w = widget(nid);
    if (!w || depth >= MAX_DEPTH)
        return false;
    for (int i = 0; i < depth; i++)
        if (p->nid[i] == nid)
            return false; /* loop */
    p->nid[depth] = nid;
    p->len = depth + 1;
    if (depth > 0 && match(w, arg))
        return true;
    if (depth > 0 && w->type != WT_MIXER && w->type != WT_SELECTOR)
        return false;
    for (int i = 0; i < w->nconn; i++) {
        p->idx[depth] = (uint8_t)i;
        if (find_path(p, depth + 1, w->conn[i], match, arg))
            return true;
    }
    return false;
}

/* 0 dB with both channels unmuted: "offset" is the step that means 0 dB. */
static void amp_unmute(struct widget *w, bool output, int index, bool mute) {
    uint32_t caps = (w->caps & WCAP_AMP_OVRD) ? (output ? w->amp_out : w->amp_in)
                                               : (output ? hda.afg_amp_out : hda.afg_amp_in);
    uint32_t gain = caps & 0x7f;
    uint32_t v = (output ? 0x8000u : 0x4000u) | 0x3000u | ((uint32_t)(index & 0xf) << 8) | (mute ? 0x80u : 0) | gain;
    codec_cmd(w->nid, (VERB4_SET_AMP << 16) | v);
}

/* Powers, routes and unmutes every widget on `p`. nid[i] selects nid[i+1]
 * through its connection idx[i]. */
static void setup_path(const struct hda_path *p) {
    for (int i = 0; i < p->len; i++) {
        struct widget *w = widget(p->nid[i]);
        verb12(w->nid, VERB_SET_POWER, 0); /* D0 */
        if (i + 1 < p->len) {
            int sel = p->idx[i];
            if (w->type == WT_MIXER) {
                /* A mixer sums all its inputs: mute the others so e.g. the
                 * analog mic loopback does not feed the speakers. */
                if (w->caps & WCAP_IN_AMP)
                    for (int k = 0; k < w->nconn; k++)
                        amp_unmute(w, false, k, k != sel);
            } else {
                if (w->nconn > 1)
                    verb12(w->nid, VERB_SET_CONN_SEL, (uint8_t)sel);
                if (w->caps & WCAP_IN_AMP)
                    amp_unmute(w, false, w->type == WT_SELECTOR || w->type == WT_ADC ? sel : 0, false);
            }
        } else if (w->type == WT_PIN && (w->caps & WCAP_IN_AMP)) {
            amp_unmute(w, false, 0, false); /* input pin boost at 0 dB */
        }
        if (w->caps & WCAP_OUT_AMP)
            amp_unmute(w, true, 0, false);
    }
}

static void setup_converter(uint16_t nid, uint8_t tag) {
    verb12(nid, VERB_SET_STREAM, (uint8_t)(tag << 4)); /* channel 0 (left), 1 follows */
    codec_cmd(nid, (VERB4_SET_FORMAT << 16) | FMT_48K_S16_2);
}

static const char *dev_name(uint8_t dev) {
    switch (dev) {
    case DEV_LINE_OUT: return "line out";
    case DEV_SPEAKER: return "speaker";
    case DEV_HP_OUT: return "headphones";
    case DEV_LINE_IN: return "line in";
    case DEV_MIC_IN: return "mic";
    }
    return "other";
}

static const char *vendor_name(uint16_t v) {
    switch (v) {
    case 0x1af4: return "QEMU HDA";
    case 0x10ec: return "Realtek";
    case 0x8086: return "Intel";
    case 0x14f1: return "Conexant";
    case 0x111d: case 0x8384: return "IDT/Sigmatel";
    case 0x11d4: return "Analog Devices";
    case 0x1106: return "VIA";
    case 0x1013: return "Cirrus Logic";
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD";
    }
    return "unknown";
}

struct codec_result {
    uint16_t out_pin, dac, in_pin, adc;
    int extra_outs;
};

/* Reads codec `cad`'s audio function group and sets up its paths. Returns
 * false if it has no usable output (e.g. an HDMI-only codec). */
static bool probe_codec(uint8_t cad, struct codec_result *res) {
    hda.cad = cad;
    memset(res, 0, sizeof *res);
    uint32_t nodes = param(0, PARAM_NODE_COUNT);
    uint16_t start = (nodes >> 16) & 0xff, count = nodes & 0xff;
    hda.afg = 0;
    for (uint16_t n = start; n < start + count; n++)
        if ((param(n, PARAM_FG_TYPE) & 0xff) == 0x01) {
            hda.afg = n;
            break;
        }
    if (!hda.afg)
        return false;
    verb12(hda.afg, VERB_SET_POWER, 0);
    udelay(1000); /* power-up from D3 can take a moment */
    hda.afg_amp_in = param(hda.afg, PARAM_AMP_IN);
    hda.afg_amp_out = param(hda.afg, PARAM_AMP_OUT);
    nodes = param(hda.afg, PARAM_NODE_COUNT);
    hda.first_nid = (nodes >> 16) & 0xff;
    hda.nw = nodes & 0xff;
    if (hda.nw == 0)
        return false;
    if (hda.w)
        kfree(hda.w);
    hda.w = kzalloc(sizeof(struct widget) * (size_t)hda.nw);
    if (!hda.w)
        return false;
    for (int i = 0; i < hda.nw; i++) {
        struct widget *w = &hda.w[i];
        w->nid = (uint16_t)(hda.first_nid + i);
        w->caps = param(w->nid, PARAM_WCAPS);
        w->type = (w->caps >> 20) & 0xf;
        if (w->caps & WCAP_AMP_OVRD) {
            w->amp_in = param(w->nid, PARAM_AMP_IN);
            w->amp_out = param(w->nid, PARAM_AMP_OUT);
        }
        if (w->type == WT_PIN) {
            w->pincaps = param(w->nid, PARAM_PINCAPS);
            w->config = verb12(w->nid, VERB_GET_CONFIG, 0);
        }
        if (w->caps & (1u << 8)) /* has a connection list */
            read_connections(w);
        if (hda.cmd_timeouts > 8)
            return false;
    }

    /* Outputs: every connected output pin gets a route to a DAC. All DACs
     * used listen to the same stream tag, so speakers, headphones and line
     * out all play (there is no jack sensing yet). The primary pin, used
     * for the log, is the most likely main output. */
    static const uint8_t out_prio[] = {DEV_SPEAKER, DEV_LINE_OUT, DEV_HP_OUT};
    for (size_t pr = 0; pr < ARRAY_LEN(out_prio); pr++) {
        for (int i = 0; i < hda.nw; i++) {
            struct widget *w = &hda.w[i];
            if (w->type != WT_PIN || !(w->pincaps & PINCAP_OUT) || !pin_connected(w) ||
                pin_device(w) != out_prio[pr])
                continue;
            struct hda_path p;
            /* Prefer the DAC already in use, so outputs share converters. */
            if (!(res->dac && find_path(&p, 0, w->nid, match_dac, res->dac)) &&
                !find_path(&p, 0, w->nid, match_dac, 0))
                continue;
            setup_path(&p);
            uint16_t dac = p.nid[p.len - 1];
            setup_converter(dac, OUT_TAG);
            uint8_t ctl = 0x40; /* output enable */
            if (pin_device(w) == DEV_HP_OUT && (w->pincaps & PINCAP_HP))
                ctl |= 0x80; /* headphone amplifier */
            verb12(w->nid, VERB_SET_PIN_CTL, ctl);
            if (w->pincaps & PINCAP_EAPD)
                verb12(w->nid, VERB_SET_EAPD, 0x02); /* external amplifier on */
            if (!res->out_pin) {
                res->out_pin = w->nid;
                res->dac = dac;
            } else {
                res->extra_outs++;
            }
        }
    }
    if (!res->out_pin)
        return false;

    /* Input: a microphone if there is one, else line in. */
    static const uint8_t in_prio[] = {DEV_MIC_IN, DEV_LINE_IN, 0xff};
    for (size_t pr = 0; pr < ARRAY_LEN(in_prio) && !res->in_pin; pr++) {
        for (int i = 0; i < hda.nw && !res->in_pin; i++) {
            struct widget *w = &hda.w[i];
            struct hda_path p;
            if (w->type != WT_ADC || !find_path(&p, 0, w->nid, match_in_pin, in_prio[pr]))
                continue;
            setup_path(&p);
            setup_converter(w->nid, IN_TAG);
            struct widget *pin = widget(p.nid[p.len - 1]);
            uint8_t ctl = 0x20; /* input enable */
            if (pin_device(pin) == DEV_MIC_IN) { /* bias voltage for electret mics */
                if (pin->pincaps & PINCAP_VREF80)
                    ctl |= 0x04;
                else if (pin->pincaps & PINCAP_VREF50)
                    ctl |= 0x01;
            }
            verb12(pin->nid, VERB_SET_PIN_CTL, ctl);
            res->in_pin = pin->nid;
            res->adc = w->nid;
        }
    }
    return true;
}

/* ---- streams ---- */

static bool stream_init(struct stream *s, bool output, int index, uint8_t tag) {
    s->present = true;
    s->output = output;
    s->index = index;
    s->tag = tag;
    s->sd = 0x80 + 0x20 * (uint32_t)index;
    s->ring = dma_alloc(RING, &s->ring_phys);
    s->bdl = dma_alloc(sizeof(struct bdl_entry) * NBDL, &s->bdl_phys);
    if (!s->ring || !s->bdl)
        return false;
    for (int i = 0; i < NBDL; i++) {
        s->bdl[i].addr = s->ring_phys + (uint64_t)i * PERIOD;
        s->bdl[i].len = PERIOD;
        s->bdl[i].flags = 1;
    }
    return true;
}

/* Resets the descriptor, which also zeroes LPIB. */
static void stream_reset(struct stream *s) {
    wr8(s->sd + SD_CTL, 0);
    wait8(s->sd + SD_CTL, SD_CTL_RUN, 0, 1000);
    wr8(s->sd + SD_CTL, SD_CTL_SRST);
    wait8(s->sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 1000);
    wr8(s->sd + SD_CTL, 0);
    wait8(s->sd + SD_CTL, SD_CTL_SRST, 0, 1000);
    wr8(s->sd + SD_STS, SD_STS_MASK);
}

/* Called with hda.lock held. */
static void stream_start(struct stream *s) {
    if (s->running)
        return;
    stream_reset(s);
    wr32(s->sd + SD_BDPL, (uint32_t)s->bdl_phys);
    wr32(s->sd + SD_BDPU, (uint32_t)(s->bdl_phys >> 32));
    wr32(s->sd + SD_CBL, RING);
    wr16(s->sd + SD_LVI, NBDL - 1);
    wr16(s->sd + SD_FMT, FMT_48K_S16_2);
    wr8(s->sd + SD_CTL + 2, (uint8_t)(s->tag << 4));
    uint16_t fifos = rd16(s->sd + SD_FIFOS);
    s->guard = s->output ? 0 : (uint32_t)ALIGN_UP(fifos < 256 ? fifos : 256, FRAME);
    s->last_lpib = 0;
    s->hw_pos = 0;
    s->dead = false;
    s->progress_ns = time_monotonic_ns();
    s->idle_ns = 0;
    s->running = true;
    if (!s->output)
        s->app_pos = 0;
    wr8(s->sd + SD_CTL, (uint8_t)(SD_CTL_RUN | (hda.irq ? SD_CTL_IOCE : 0)));
    waitq_wake_all(&hda.kick);
}

/* Called with hda.lock held. Discards anything queued. */
static void stream_stop(struct stream *s) {
    if (!s->running && !s->app_pos)
        return;
    stream_reset(s);
    s->running = false;
    s->dead = false;
    s->hw_pos = s->app_pos = 0;
    s->last_lpib = 0;
    memset(s->ring, 0, RING);
    waitq_wake_all(&hda.wq);
}

/* Brings hw_pos up to date. Called with hda.lock held, from the interrupt
 * handler, the service thread and the file operations. */
static bool stream_update(struct stream *s, uint64_t now) {
    if (!s->running)
        return false;
    uint64_t delta;
    if (s->dead && !s->output) {
        /* A capture source can be slow to start (a host sound server, say):
         * if the hardware moves after all, carry on from its position. */
        uint32_t lpib = rd32(s->sd + SD_LPIB) % RING;
        if (lpib != s->last_lpib) {
            s->hw_pos = s->app_pos + (lpib + RING - (uint32_t)(s->app_pos % RING)) % RING;
            s->last_lpib = lpib;
            s->dead = false;
            s->progress_ns = now;
            return true;
        }
    }
    if (s->dead) {
        uint64_t target = s->dead_pos + elapsed(now, s->dead_ns) * BYTES_PER_SEC / 1000000000ULL;
        target = ALIGN_DOWN(target, FRAME);
        delta = target > s->hw_pos ? target - s->hw_pos : 0;
    } else {
        /* Acknowledge buffer completion here as well as in the interrupt
         * handler: QEMU holds a stream's DMA until the guest clears BCIS,
         * so one lost or late interrupt (or polled mode, where the BDL
         * still asks for completions) would otherwise stall playback. */
        if (rd8(s->sd + SD_STS) & SD_STS_MASK)
            wr8(s->sd + SD_STS, SD_STS_MASK);
        uint32_t lpib = rd32(s->sd + SD_LPIB) % RING;
        delta = (lpib + RING - s->last_lpib) % RING;
        s->last_lpib = lpib;
        if (delta) {
            s->progress_ns = now;
        } else if (elapsed(now, s->progress_ns) > (s->output ? OUT_STALL_NS : STALL_NS)) {
            /* Nothing behind the codec: simulate the clock from here on. */
            kprintf("hda: %s stream is not moving, continuing with silence\n", s->output ? "output" : "input");
            if (s->output) /* stays dead until the stream next stops */
                wr8(s->sd + SD_CTL, 0);
            else /* captured data now comes from the zeroed ring */
                memset(s->ring, 0, RING);
            s->dead = true;
            s->dead_ns = now;
            s->dead_pos = s->hw_pos;
            return false;
        }
    }
    if (!delta)
        return false;
    if (s->output) {
        /* Zero what was just played: an underrun then plays silence. */
        uint32_t off = (uint32_t)(s->hw_pos % RING);
        uint64_t n = delta > RING ? RING : delta;
        uint32_t first = (uint32_t)(n < RING - off ? n : RING - off);
        memset(s->ring + off, 0, first);
        memset(s->ring, 0, n - first);
    }
    s->hw_pos += delta;
    return true;
}

static void service(void) {
    uint64_t now = time_monotonic_ns();
    uint64_t f = spin_lock_irqsave(&hda.lock);
    bool moved = stream_update(&hda.out, now);
    moved |= stream_update(&hda.in, now);
    spin_unlock_irqrestore(&hda.lock, f);
    if (moved)
        waitq_wake_all(&hda.wq);
}

static void hda_irq(struct regs *r) {
    (void)r;
    uint32_t sts = rd32(INTSTS);
    if (hda.out.present && (sts & (1u << hda.out.index)))
        wr8(hda.out.sd + SD_STS, SD_STS_MASK);
    if (hda.in.present && (sts & (1u << hda.in.index)))
        wr8(hda.in.sd + SD_STS, SD_STS_MASK);
    service();
    lapic_eoi();
}

static bool streams_running(void *arg) {
    (void)arg;
    return hda.out.running || hda.in.running;
}

/* Polls LPIB while a stream runs (so a missing interrupt never stalls a
 * program) and stops output that has gone idle. */
static void hda_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (!streams_running(NULL)) {
            waitq_wait_cond(&hda.kick, streams_running, NULL);
            continue;
        }
        service();
        uint64_t now = time_monotonic_ns();
        uint64_t f = spin_lock_irqsave(&hda.lock);
        struct stream *o = &hda.out;
        if (!o->running || o->app_pos > o->hw_pos)
            o->idle_ns = 0;
        else if (!o->idle_ns)
            o->idle_ns = now; /* the hardware just ran out of data */
        else if (elapsed(now, o->idle_ns) > IDLE_STOP_NS)
            stream_stop(o);
        spin_unlock_irqrestore(&hda.lock, f);
        thread_sleep_ms(8);
    }
}

/* ---- sample rate conversion ---- */

/* Stereo 16-bit frames in, stereo 16-bit frames out. Upsampling uses linear
 * interpolation; downsampling averages the input frames that fall into each
 * output frame (a box filter, which keeps the worst aliasing out of speech
 * captured at 16 kHz). */
struct resampler {
    uint32_t in_rate, out_rate;
    uint32_t pos;       /* up: Q16 position between prev and cur */
    int32_t prev[2], cur[2];
    bool primed;
    int32_t acc[2];     /* down: running sum */
    uint32_t count;
    uint64_t phase;
};

static void rs_init(struct resampler *r, uint32_t in_rate, uint32_t out_rate) {
    memset(r, 0, sizeof *r);
    r->in_rate = in_rate;
    r->out_rate = out_rate;
}

/* Converts until the input is used up or the output is full. Returns frames
 * produced; *used gets the input frames consumed. */
static size_t rs_run(struct resampler *r, const int16_t *in, size_t n_in, int16_t *out, size_t cap,
                     size_t *used) {
    size_t i = 0, o = 0;
    if (r->in_rate == r->out_rate) {
        size_t n = n_in < cap ? n_in : cap;
        memcpy(out, in, n * FRAME);
        *used = n;
        return n;
    }
    if (r->in_rate < r->out_rate) {
        uint32_t step = (uint32_t)(((uint64_t)r->in_rate << 16) / r->out_rate);
        if (!r->primed && n_in) {
            r->prev[0] = r->cur[0] = in[0];
            r->prev[1] = r->cur[1] = in[1];
            r->pos = 0;
            r->primed = true;
            i = 1;
        }
        for (;;) {
            while (r->pos < 65536) {
                if (o == cap)
                    goto done;
                for (int c = 0; c < 2; c++)
                    out[o * 2 + c] =
                        (int16_t)(r->prev[c] + (int32_t)(((int64_t)(r->cur[c] - r->prev[c]) * r->pos) >> 16));
                o++;
                r->pos += step;
            }
            if (i == n_in)
                goto done;
            r->prev[0] = r->cur[0], r->prev[1] = r->cur[1];
            r->cur[0] = in[i * 2], r->cur[1] = in[i * 2 + 1];
            i++;
            r->pos -= 65536;
        }
    }
    while (i < n_in && o < cap) {
        r->acc[0] += in[i * 2];
        r->acc[1] += in[i * 2 + 1];
        r->count++;
        i++;
        r->phase += r->out_rate;
        if (r->phase >= r->in_rate) {
            r->phase -= r->in_rate;
            out[o * 2] = (int16_t)(r->acc[0] / (int32_t)r->count);
            out[o * 2 + 1] = (int16_t)(r->acc[1] / (int32_t)r->count);
            o++;
            r->acc[0] = r->acc[1] = 0;
            r->count = 0;
        }
    }
done:
    *used = i;
    return o;
}

/* ---- /dev/dsp (OSS) ---- */

#define SNDCTL_DSP_RESET       0x5000
#define SNDCTL_DSP_SYNC        0x5001
#define SNDCTL_DSP_SPEED       0xc0045002
#define SNDCTL_DSP_STEREO      0xc0045003
#define SNDCTL_DSP_GETBLKSIZE  0xc0045004
#define SNDCTL_DSP_SETFMT      0xc0045005
#define SNDCTL_DSP_CHANNELS    0xc0045006
#define SNDCTL_DSP_POST        0x5008
#define SNDCTL_DSP_SETFRAGMENT 0xc004500a
#define SNDCTL_DSP_GETFMTS     0x8004500b
#define SNDCTL_DSP_GETOSPACE   0x8010500c
#define SNDCTL_DSP_GETISPACE   0x8010500d
#define SNDCTL_DSP_NONBLOCK    0x500e
#define SNDCTL_DSP_GETCAPS     0x8004500f
#define SNDCTL_DSP_GETTRIGGER  0x80045010
#define SNDCTL_DSP_SETTRIGGER  0x40045010
#define SNDCTL_DSP_GETODELAY   0x80045017
#define SOUND_PCM_READ_RATE    0x80045002
#define SOUND_PCM_READ_CHANNELS 0x80045006
#define SOUND_PCM_READ_BITS    0x80045005
#define OSS_GETVERSION         0x80044d76
#define AFMT_S16_LE            0x10
#define DSP_CAP_DUPLEX         0x100
#define DSP_CAP_TRIGGER        0x1000
#define PCM_ENABLE_INPUT       1
#define PCM_ENABLE_OUTPUT      2

struct audio_buf_info {
    int fragments, fragstotal, fragsize, bytes;
};

#define BLOCK_IN  256  /* program frames converted per step on output */
#define BLOCK_CAP 1024 /* hardware frames converted per step on input */
#define STAGE     (BLOCK_CAP * 4 + 8) /* frames: enough for 8 kHz -> 48 kHz and 48 -> 192 kHz */

struct dsp_file {
    bool rd, wr;
    uint32_t rate, channels;
    struct resampler out_rs, in_rs;
    uint8_t carry[4];
    uint32_t carry_len;
    int16_t tmp[BLOCK_CAP * 2];
    int16_t stage[STAGE * 2];
};

static uint32_t app_frame(const struct dsp_file *d) { return 2 * d->channels; }

/* hardware bytes -> program bytes (whole frames) */
static uint64_t to_app_bytes(const struct dsp_file *d, uint64_t hw) {
    return hw / FRAME * d->rate / HW_RATE * app_frame(d);
}

/* Q16 gain from an OSS level (0-100). Squared: loudness follows the slider
 * more naturally than amplitude does. */
static uint32_t gain_q16(uint32_t level) {
    if (level > 100)
        level = 100;
    return level * level * 65536 / 10000;
}

static void out_gains(uint32_t g[2]) {
    for (int c = 0; c < 2; c++) {
        uint32_t m = (hda.vol_master >> (8 * c)) & 0xff, p = (hda.vol_pcm >> (8 * c)) & 0xff;
        g[c] = (uint32_t)(((uint64_t)gain_q16(m) * gain_q16(p)) >> 16);
    }
}

static void set_rates(struct dsp_file *d) {
    rs_init(&d->out_rs, d->rate, HW_RATE);
    rs_init(&d->in_rs, HW_RATE, d->rate);
    d->carry_len = 0;
}

static uint64_t out_fill(const struct stream *s) { return s->app_pos > s->hw_pos ? s->app_pos - s->hw_pos : 0; }

static bool out_has_space(void *arg) {
    uint64_t need = (uint64_t)(uintptr_t)arg;
    return RING - out_fill(&hda.out) >= need || !hda.out.present;
}

/* Queues `n` program frames. Returns frames taken or a negative errno. */
static int64_t push_frames(struct dsp_file *d, const int16_t *in, size_t n, bool nonblock) {
    struct stream *s = &hda.out;
    size_t done = 0;
    uint32_t g[2];
    while (done < n) {
        size_t blk = n - done < BLOCK_IN ? n - done : BLOCK_IN;
        uint64_t need = ((uint64_t)blk * HW_RATE / d->rate + 3) * FRAME;
        uint64_t now = time_monotonic_ns();
        uint64_t f = spin_lock_irqsave(&hda.lock);
        stream_update(s, now);
        if (s->running && s->app_pos < s->hw_pos + FRAME) /* underrun: restart just ahead */
            s->app_pos = ALIGN_UP(s->hw_pos + UNDERRUN_GAP, FRAME);
        if (RING - out_fill(s) < need) {
            spin_unlock_irqrestore(&hda.lock, f);
            if (nonblock)
                return done ? (int64_t)done : -EAGAIN;
            if (waitq_wait_cond(&hda.wq, out_has_space, (void *)(uintptr_t)need) < 0)
                return done ? (int64_t)done : -EINTR;
            continue;
        }
        /* program channels -> stereo */
        const int16_t *src = in + done * d->channels;
        int16_t *st = d->tmp;
        for (size_t i = 0; i < blk; i++) {
            st[i * 2] = src[i * d->channels];
            st[i * 2 + 1] = d->channels == 2 ? src[i * 2 + 1] : src[i];
        }
        size_t used;
        size_t got = rs_run(&d->out_rs, st, blk, d->stage, STAGE, &used);
        out_gains(g);
        for (size_t i = 0; i < got; i++) {
            uint32_t off = (uint32_t)((s->app_pos + i * FRAME) % RING);
            int16_t *dst = (int16_t *)(s->ring + off);
            dst[0] = (int16_t)(((int32_t)d->stage[i * 2] * (int32_t)g[0]) >> 16);
            dst[1] = (int16_t)(((int32_t)d->stage[i * 2 + 1] * (int32_t)g[1]) >> 16);
        }
        s->app_pos += got * FRAME;
        s->idle_ns = 0;
        if (!s->running && out_fill(s) >= PERIOD)
            stream_start(s);
        spin_unlock_irqrestore(&hda.lock, f);
        done += used;
    }
    return (int64_t)done;
}

static int64_t dsp_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v, (void)off;
    struct dsp_file *d = f->private;
    if (!d || !d->wr)
        return -EBADF;
    bool nonblock = f->flags & O_NONBLOCK;
    uint32_t fs = app_frame(d);
    const uint8_t *p = buf;
    size_t left = len, taken = 0;
    /* A frame split across two writes: finish it first. */
    if (d->carry_len) {
        size_t k = fs - d->carry_len < left ? fs - d->carry_len : left;
        memcpy(d->carry + d->carry_len, p, k);
        if (d->carry_len + k < fs) {
            d->carry_len += (uint32_t)k;
            return (int64_t)len;
        }
        int64_t r = push_frames(d, (const int16_t *)d->carry, 1, nonblock);
        if (r <= 0)
            return r;
        d->carry_len = 0;
        p += k, left -= k, taken += k;
    }
    size_t frames = left / fs;
    if (frames) {
        int16_t aligned[256]; /* the bounce buffer is aligned, but be safe */
        const int16_t *src = (const int16_t *)p;
        size_t done = 0;
        while (done < frames) {
            size_t chunk = frames - done;
            int64_t r;
            if ((uintptr_t)p & 1) {
                chunk = chunk < sizeof aligned / fs ? chunk : sizeof aligned / fs;
                memcpy(aligned, p + done * fs, chunk * fs);
                r = push_frames(d, aligned, chunk, nonblock);
            } else {
                r = push_frames(d, src + done * d->channels, chunk, nonblock);
            }
            if (r <= 0) {
                if (taken || done)
                    break;
                return r;
            }
            done += (size_t)r;
            if ((size_t)r < chunk)
                break;
        }
        taken += done * fs;
        if (done < frames)
            return (int64_t)taken;
        p += done * fs, left -= done * fs;
    }
    if (left) { /* keep the partial frame for next time */
        memcpy(d->carry, p, left);
        d->carry_len = (uint32_t)left;
        taken += left;
    }
    return (int64_t)taken;
}

static uint64_t in_avail(const struct stream *s) {
    uint64_t hw = s->hw_pos > s->guard ? s->hw_pos - s->guard : 0;
    return hw > s->app_pos ? hw - s->app_pos : 0;
}

static bool in_has_data(void *arg) {
    (void)arg;
    return in_avail(&hda.in) >= FRAME || !hda.in.running;
}

static void capture_start(void) {
    if (!hda.in.present)
        return;
    uint64_t f = spin_lock_irqsave(&hda.lock);
    if (!hda.in.running)
        stream_start(&hda.in);
    spin_unlock_irqrestore(&hda.lock, f);
}

static int64_t dsp_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)off;
    struct dsp_file *d = f->private;
    if (!d || !d->rd)
        return -EBADF;
    if (!hda.in.present)
        return -ENODEV;
    bool nonblock = f->flags & O_NONBLOCK;
    uint32_t fs = app_frame(d);
    size_t want = len / fs, got = 0;
    if (!want)
        return -EINVAL;
    capture_start();
    struct stream *s = &hda.in;
    int16_t *out = buf;
    uint32_t g[2];
    for (int c = 0; c < 2; c++)
        g[c] = gain_q16((hda.vol_mic >> (8 * c)) & 0xff);
    /* Blocking reads return the whole request, as OSS does. */
    while (got < want) {
        uint64_t f2 = spin_lock_irqsave(&hda.lock);
        stream_update(s, time_monotonic_ns());
        uint64_t avail = in_avail(s);
        if (avail > RING - PERIOD) { /* overrun: the hardware lapped us, skip ahead */
            s->app_pos += ALIGN_UP(avail - (RING - PERIOD), FRAME);
            avail = in_avail(s);
        }
        if (avail < FRAME) {
            spin_unlock_irqrestore(&hda.lock, f2);
            if (nonblock)
                break;
            if (!s->running)
                capture_start();
            if (waitq_wait_cond(&hda.wq, in_has_data, NULL) < 0) {
                if (!got)
                    return -EINTR;
                break;
            }
            continue;
        }
        size_t blk = avail / FRAME < BLOCK_CAP ? avail / FRAME : BLOCK_CAP;
        for (size_t i = 0; i < blk; i++) {
            uint32_t o = (uint32_t)((s->app_pos + i * FRAME) % RING);
            d->tmp[i * 2] = *(int16_t *)(s->ring + o);
            d->tmp[i * 2 + 1] = *(int16_t *)(s->ring + o + 2);
        }
        size_t cap = want - got < STAGE ? want - got : STAGE;
        size_t used;
        size_t n = rs_run(&d->in_rs, d->tmp, blk, d->stage, cap, &used);
        s->app_pos += used * FRAME;
        for (size_t i = 0; i < n; i++) {
            int32_t l = ((int32_t)d->stage[i * 2] * (int32_t)g[0]) >> 16;
            int32_t r = ((int32_t)d->stage[i * 2 + 1] * (int32_t)g[1]) >> 16;
            if (d->channels == 2) {
                out[(got + i) * 2] = (int16_t)l;
                out[(got + i) * 2 + 1] = (int16_t)r;
            } else {
                out[got + i] = (int16_t)((l + r) / 2);
            }
        }
        got += n;
        spin_unlock_irqrestore(&hda.lock, f2);
    }
    if (!got)
        return -EAGAIN;
    return (int64_t)(got * fs);
}

/* Waits until the hardware has taken everything queued. Bounded, so a dead
 * device or a killed process cannot hang here.
 *
 * The stream is deliberately left running (on silence) rather than stopped:
 * the codec side still holds audio that LPIB has already passed (a FIFO on
 * real hardware; with QEMU, its codec buffer and audio backend, 75 ms or
 * more), and stopping now would cut the end of the sound off. The service
 * thread stops the stream once it has been idle for IDLE_STOP_NS. */
static int drain(bool interruptible) {
    struct stream *s = &hda.out;
    uint64_t f = spin_lock_irqsave(&hda.lock);
    if (!s->running && s->app_pos)
        stream_start(s);
    uint64_t target = s->app_pos;
    uint64_t fill = out_fill(s);
    spin_unlock_irqrestore(&hda.lock, f);
    uint64_t deadline = time_monotonic_ns() + fill * 1000000000ULL / BYTES_PER_SEC + 1000000000ULL;
    for (;;) {
        service();
        if (!s->running || s->hw_pos >= target || time_monotonic_ns() > deadline)
            return 0;
        if (interruptible && signal_pending_current())
            return -EINTR;
        thread_sleep_ms(4);
    }
}

static int dsp_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)v;
    struct dsp_file *d = f->private;
    if (wq)
        *wq = &hda.wq;
    if (!d)
        return POLLERR;
    int ev = 0;
    service();
    if (d->wr && RING - out_fill(&hda.out) >= PERIOD)
        ev |= POLLOUT;
    if (d->rd) {
        if (!hda.in.running)
            capture_start(); /* polling for input is how programs start recording */
        if (in_avail(&hda.in) >= FRAME)
            ev |= POLLIN;
    }
    return ev;
}

static int64_t put_int(uint64_t arg, int v) { return copy_to_user(arg, &v, sizeof v); }

static int64_t dsp_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v;
    struct dsp_file *d = f->private;
    if (!d)
        return -EBADF;
    int val = 0;
    if ((req >> 30) & 1) { /* the caller passes a value in */
        if (copy_from_user(&val, arg, sizeof val) < 0)
            return -EFAULT;
    }
    uint64_t fl;
    switch (req) {
    case SNDCTL_DSP_RESET:
        fl = spin_lock_irqsave(&hda.lock);
        if (d->wr)
            stream_stop(&hda.out);
        if (d->rd)
            stream_stop(&hda.in);
        spin_unlock_irqrestore(&hda.lock, fl);
        set_rates(d);
        return 0;
    case SNDCTL_DSP_SYNC:
        return d->wr ? drain(true) : 0;
    case SNDCTL_DSP_POST:
        fl = spin_lock_irqsave(&hda.lock);
        if (d->wr && !hda.out.running && hda.out.app_pos)
            stream_start(&hda.out);
        spin_unlock_irqrestore(&hda.lock, fl);
        return 0;
    case SNDCTL_DSP_SPEED:
        /* Any rate works: the driver converts to and from 48 kHz. */
        if (val > 0) {
            d->rate = (uint32_t)(val < 8000 ? 8000 : val > 192000 ? 192000 : val);
            set_rates(d);
        }
        return put_int(arg, (int)d->rate);
    case SOUND_PCM_READ_RATE:
        return put_int(arg, (int)d->rate);
    case SNDCTL_DSP_STEREO:
        d->channels = val ? 2 : 1;
        set_rates(d);
        return put_int(arg, val ? 1 : 0);
    case SNDCTL_DSP_CHANNELS:
        if (val > 0) {
            d->channels = val == 1 ? 1 : 2;
            set_rates(d);
        }
        return put_int(arg, (int)d->channels);
    case SOUND_PCM_READ_CHANNELS:
        return put_int(arg, (int)d->channels);
    case SNDCTL_DSP_SETFMT:      /* only signed 16-bit little-endian */
    case SNDCTL_DSP_GETFMTS:
        return put_int(arg, AFMT_S16_LE);
    case SOUND_PCM_READ_BITS:
        return put_int(arg, 16);
    case SNDCTL_DSP_GETBLKSIZE:
        return put_int(arg, (int)to_app_bytes(d, PERIOD));
    case SNDCTL_DSP_SETFRAGMENT: /* fixed fragments; accepted for compatibility */
        return 0;
    case SNDCTL_DSP_NONBLOCK:
        f->flags |= O_NONBLOCK;
        return 0;
    case SNDCTL_DSP_GETCAPS:
        return put_int(arg, DSP_CAP_DUPLEX | DSP_CAP_TRIGGER | 1);
    case SNDCTL_DSP_GETTRIGGER:
        return put_int(arg, (hda.in.running ? PCM_ENABLE_INPUT : 0) | (hda.out.running ? PCM_ENABLE_OUTPUT : 0));
    case SNDCTL_DSP_SETTRIGGER:
        fl = spin_lock_irqsave(&hda.lock);
        if (d->rd && hda.in.present) {
            if (val & PCM_ENABLE_INPUT)
                stream_start(&hda.in);
            else
                stream_stop(&hda.in);
        }
        if (d->wr && (val & PCM_ENABLE_OUTPUT) && !hda.out.running && hda.out.app_pos)
            stream_start(&hda.out);
        spin_unlock_irqrestore(&hda.lock, fl);
        return 0;
    case SNDCTL_DSP_GETOSPACE:
    case SNDCTL_DSP_GETISPACE: {
        service();
        struct audio_buf_info info;
        uint64_t hw = req == SNDCTL_DSP_GETOSPACE ? RING - out_fill(&hda.out) : in_avail(&hda.in);
        info.fragsize = (int)to_app_bytes(d, PERIOD);
        info.fragstotal = NBDL;
        info.bytes = (int)to_app_bytes(d, hw);
        info.fragments = info.fragsize ? info.bytes / info.fragsize : 0;
        return copy_to_user(arg, &info, sizeof info);
    }
    case SNDCTL_DSP_GETODELAY:
        service();
        return put_int(arg, (int)to_app_bytes(d, out_fill(&hda.out)));
    case OSS_GETVERSION:
        return put_int(arg, 0x030000);
    }
    return -ENOTTY;
}

static int dsp_open(struct vnode *v, struct file *f) {
    (void)v;
    uint32_t acc = f->flags & O_ACCMODE;
    bool rd = acc == O_RDONLY || acc == O_RDWR, wr = acc == O_WRONLY || acc == O_RDWR;
    if (rd && !hda.in.present && !wr)
        return -ENODEV;
    struct dsp_file *d = kzalloc(sizeof *d);
    if (!d)
        return -ENOMEM;
    /* OSS defaults to 8 kHz mono 8-bit; we only do 16-bit, and 48 kHz
     * stereo is the format every caller here asks for anyway. */
    d->rate = HW_RATE;
    d->channels = 2;
    d->rd = rd && hda.in.present;
    d->wr = wr;
    set_rates(d);
    uint64_t fl = spin_lock_irqsave(&hda.lock);
    /* One player and one recorder at a time (there is no software mixer). */
    if ((d->wr && hda.writer) || (d->rd && hda.reader)) {
        spin_unlock_irqrestore(&hda.lock, fl);
        kfree(d);
        return -EBUSY;
    }
    if (d->wr)
        hda.writer = d;
    if (d->rd)
        hda.reader = d;
    spin_unlock_irqrestore(&hda.lock, fl);
    f->private = d;
    return 0;
}

static void dsp_close(struct vnode *v, struct file *f) {
    (void)v;
    struct dsp_file *d = f->private;
    if (!d)
        return;
    if (d->wr)
        drain(false); /* closing waits for the sound to finish, as OSS does */
    uint64_t fl = spin_lock_irqsave(&hda.lock);
    if (d->wr) /* output winds down by itself (see drain) */
        hda.writer = NULL;
    if (d->rd) {
        stream_stop(&hda.in);
        hda.reader = NULL;
    }
    spin_unlock_irqrestore(&hda.lock, fl);
    f->private = NULL;
    kfree(d);
}

static const struct vnode_ops dsp_ops = {
    .read = dsp_read, .write = dsp_write, .ioctl = dsp_ioctl, .poll = dsp_poll, .open = dsp_open, .close = dsp_close,
};

/* ---- /dev/mixer: master, PCM and microphone levels (software gain) ---- */

#define SOUND_MIXER_VOLUME 0
#define SOUND_MIXER_PCM    4
#define SOUND_MIXER_MIC    7

static int64_t mixer_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v, (void)f;
    if (((req >> 8) & 0xff) != 'M')
        return -ENOTTY;
    uint32_t nr = req & 0xff;
    bool write = (req >> 30) & 1;
    if (req == OSS_GETVERSION)
        return put_int(arg, 0x030000);
    switch (nr) {
    case 0xfe: /* DEVMASK */
        return put_int(arg, (1 << SOUND_MIXER_VOLUME) | (1 << SOUND_MIXER_PCM) | (1 << SOUND_MIXER_MIC));
    case 0xfb: /* STEREODEVS */
        return put_int(arg, (1 << SOUND_MIXER_VOLUME) | (1 << SOUND_MIXER_PCM) | (1 << SOUND_MIXER_MIC));
    case 0xfd: /* RECMASK */
    case 0xff: /* RECSRC (writes accepted: there is only one) */
        return put_int(arg, 1 << SOUND_MIXER_MIC);
    case 0xfc: /* CAPS: one recording source at a time */
        return put_int(arg, 1);
    }
    uint32_t *level = nr == SOUND_MIXER_VOLUME ? &hda.vol_master
                      : nr == SOUND_MIXER_PCM  ? &hda.vol_pcm
                      : nr == SOUND_MIXER_MIC  ? &hda.vol_mic
                                               : NULL;
    if (!level)
        return -EINVAL;
    if (write) {
        int val;
        if (copy_from_user(&val, arg, sizeof val) < 0)
            return -EFAULT;
        uint32_t l = (uint32_t)val & 0xff, r = ((uint32_t)val >> 8) & 0xff;
        *level = (l > 100 ? 100 : l) | (r > 100 ? 100 : r) << 8;
    }
    return put_int(arg, (int)*level);
}

static const struct vnode_ops mixer_ops = {.ioctl = mixer_ioctl};

/* ---- setup ---- */

/* Message-signalled interrupts avoid the legacy INTx routing, which needs
 * the ACPI _PRT tables we don't parse yet. */
static bool enable_msi(struct pci_dev *d, uint8_t vector) {
    if (!(pci_read16(d, 0x06) & 0x10)) /* no capability list */
        return false;
    uint8_t ptr = pci_read32(d, 0x34) & 0xfc;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint32_t hdr = pci_read32(d, ptr);
        if ((hdr & 0xff) == 0x05) {
            uint16_t ctl = (uint16_t)(hdr >> 16);
            pci_write32(d, (uint8_t)(ptr + 4), 0xfee00000u | (cpus[0]->arch.lapic_id << 12));
            if (ctl & 0x80) { /* 64-bit address */
                pci_write32(d, (uint8_t)(ptr + 8), 0);
                pci_write16(d, (uint8_t)(ptr + 12), vector);
            } else {
                pci_write16(d, (uint8_t)(ptr + 8), vector);
            }
            pci_write16(d, (uint8_t)(ptr + 2), (uint16_t)((ctl & ~0x70u) | 1)); /* one vector, enabled */
            pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x400);                 /* INTx off */
            return true;
        }
        ptr = (hdr >> 8) & 0xfc;
    }
    return false;
}

static void add_node(const char *path, const struct vnode_ops *ops, uint32_t minor) {
    struct vnode *v = vnode_alloc(VN_CHR, 0666, ops);
    KASSERT(v);
    v->rdev = (14 << 8) | minor; /* the OSS major */
    vfs_create_node(path, v);
    vnode_put(v);
}

static bool bring_up(struct pci_dev *pd) {
    hda.pci = pd;
    pci_enable(pd);
    if (pd->vendor == 0x8086) /* Intel: traffic class 0, or DMA can stall (as Linux does) */
        pci_write32(pd, 0x44, pci_read32(pd, 0x44) & ~7u);
    if (pd->bar_is_io[0] || !pd->bar[0])
        return false;
    vmm_map_mmio(pd->bar[0], pd->bar_size[0] ? pd->bar_size[0] : 0x4000, true);
    hda.mmio = phys_to_virt(pd->bar[0]);

    if (!controller_reset()) {
        kprintf("hda: controller did not come out of reset\n");
        return false;
    }
    uint16_t codecs = rd16(STATESTS) & 0x7fff;
    wr16(STATESTS, codecs);
    if (!codecs) {
        kprintf("hda: no codecs\n");
        return false;
    }
    hda.use_corb = setup_corb();
    hda.cad = (uint8_t)__builtin_ctz(codecs);
    if (hda.use_corb && !param(0, PARAM_VENDOR) && hda.cmd_timeouts) {
        wr8(CORBCTL, 0); /* the rings don't answer: try immediate commands */
        wr8(RIRBCTL, 0);
        hda.use_corb = false;
        hda.cmd_timeouts = 0;
    }

    struct codec_result res = {0};
    bool found = false;
    uint32_t vendor = 0;
    for (uint8_t cad = 0; cad < 15 && !found; cad++) {
        if (!(codecs & (1u << cad)))
            continue;
        hda.cad = cad;
        hda.cmd_timeouts = 0;
        vendor = param(0, PARAM_VENDOR);
        if (!vendor || hda.cmd_timeouts)
            continue;
        found = probe_codec(cad, &res);
    }
    if (!found) {
        kprintf("hda: no codec with an analog output (codecs %x)\n", codecs);
        return false;
    }

    uint16_t gcap = rd16(GCAP);
    int iss = (gcap >> 8) & 0xf, oss = (gcap >> 12) & 0xf;
    if (!oss || !stream_init(&hda.out, true, iss, OUT_TAG))
        return false;
    if (iss && res.adc && !stream_init(&hda.in, false, 0, IN_TAG))
        hda.in.present = false;

    hda.irq = enable_msi(pd, VEC_HDA);
    if (hda.irq) {
        idt_set_handler(VEC_HDA, hda_irq);
        uint32_t mask = 1u << hda.out.index;
        if (hda.in.present)
            mask |= 1u << hda.in.index;
        wr32(INTCTL, (1u << 31) | mask); /* global enable + our two streams */
    }

    struct widget *op = widget(res.out_pin), *ip = res.in_pin ? widget(res.in_pin) : NULL;
    kprintf("hda: codec %u (%s %04x:%04x), out pin 0x%x (%s) dac 0x%x", hda.cad, vendor_name((uint16_t)(vendor >> 16)),
            vendor >> 16, vendor & 0xffff, res.out_pin, dev_name(pin_device(op)), res.dac);
    if (res.extra_outs)
        kprintf(" +%d more", res.extra_outs);
    if (ip)
        kprintf(", in pin 0x%x (%s) adc 0x%x", res.in_pin, dev_name(pin_device(ip)), res.adc);
    else
        kprintf(", no input");
    kprintf(", %s, %s\n", hda.use_corb ? "corb" : "immediate", hda.irq ? "msi" : "polled");
    return true;
}

void hda_init(void);
void hda_init(void) {
    hda.lock = (spinlock_t)SPINLOCK_INIT;
    hda.cmd_lock = (spinlock_t)SPINLOCK_INIT;
    waitq_init(&hda.wq);
    waitq_init(&hda.kick);
    hda.vol_master = hda.vol_pcm = hda.vol_mic = 100 | 100 << 8;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->class_code != 0x04 || d->subclass != 0x03)
            continue;
        if (!bring_up(d))
            continue;
        add_node("/dev/dsp", &dsp_ops, 3);
        add_node("/dev/mixer", &mixer_ops, 0);
        KASSERT(thread_create("hda", hda_thread, NULL));
        return; /* one sound card */
    }
}
