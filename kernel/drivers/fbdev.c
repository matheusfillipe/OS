#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>


/* /dev/fb0 with the Linux fbdev interface (FBIOGET_VSCREENINFO,
 * FBIOGET_FSCREENINFO, mmap). The desktop draws into it directly; once a
 * program maps it, the kernel's boot console stops drawing over it.
 * FBIOPUT_VSCREENINFO changes the resolution where the hardware allows it
 * (QEMU's std VGA, bga.c); the program then maps it again at the new size. */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock, left_margin, right_margin, upper_margin,
        lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

static struct limine_framebuffer *fb;
static uint64_t fb_phys, fb_len;
static uint32_t fb_w, fb_h, fb_pitch; /* the current mode */

#define FB_ACTIVATE_TEST 2

static void get_var(struct fb_var_screeninfo *s) {
    memset(s, 0, sizeof *s);
    s->xres = s->xres_virtual = fb_w;
    s->yres = s->yres_virtual = fb_h;
    s->bits_per_pixel = fb->bpp;
    s->red = (struct fb_bitfield){fb->red_mask_shift, fb->red_mask_size, 0};
    s->green = (struct fb_bitfield){fb->green_mask_shift, fb->green_mask_size, 0};
    s->blue = (struct fb_bitfield){fb->blue_mask_shift, fb->blue_mask_size, 0};
    s->height = s->width = 0xffffffff;
}

/* A new resolution, as Linux drivers take it: the size asked for in the
 * structure, FB_ACTIVATE_TEST to only ask whether it would work. */
static int64_t put_var(uint64_t arg) {
    struct fb_var_screeninfo s;
    if (copy_from_user(&s, arg, sizeof s) < 0)
        return -EFAULT;
    if (s.xres != fb_w || s.yres != fb_h) {
        if ((s.bits_per_pixel && s.bits_per_pixel != 32) || fb->bpp != 32 || !platform_fb_mode_fits(s.xres, s.yres))
            return -EINVAL;
        if ((s.activate & 0xf) == FB_ACTIVATE_TEST)
            return 0;
        uint64_t phys, vram;
        if (!platform_fb_set_mode(s.xres, s.yres, &phys, &vram))
            return -EINVAL;
        fb_w = s.xres, fb_h = s.yres, fb_pitch = s.xres * 4;
        fb_phys = phys;
        fb_len = vram;
        kprintf("fb: mode %ux%u\n", fb_w, fb_h);
    }
    get_var(&s);
    return copy_to_user(arg, &s, sizeof s);
}

static int64_t fb_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v;
    (void)f;
    if (req == FBIOGET_VSCREENINFO) {
        struct fb_var_screeninfo s;
        get_var(&s);
        return copy_to_user(arg, &s, sizeof s);
    }
    if (req == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo s = {0};
        memcpy(s.id, "osfb", 8);
        s.smem_start = fb_phys;
        s.smem_len = (uint32_t)fb_len;
        s.visual = 2; /* FB_VISUAL_TRUECOLOR */
        s.line_length = fb_pitch;
        return copy_to_user(arg, &s, sizeof s);
    }
    if (req == FBIOPUT_VSCREENINFO)
        return put_var(arg);
    if (req == FBIOPAN_DISPLAY)
        return 0; /* one screen's worth: nothing to pan */
    return -ENOTTY;
}

static int fb_mmap_phys(struct vnode *v, uint64_t off, uint64_t len, uint64_t *phys) {
    (void)v;
    if (off + len > ALIGN_UP(fb_len, PAGE_SIZE))
        return -EINVAL;
    *phys = fb_phys + off;
    fbcon_suspend(true); /* a program owns the screen now */
    return 0;
}

static const struct vnode_ops fb_ops = {.ioctl = fb_ioctl, .mmap_phys = fb_mmap_phys};


void fbdev_init(void) {
    struct limine_framebuffer_response *r = boot_fb_request.response;
    if (!r || !r->framebuffer_count)
        return;
    fb = r->framebuffers[0];
    fb_phys = virt_to_phys_hhdm(fb->address);
    fb_len = fb->pitch * fb->height;
    fb_w = (uint32_t)fb->width, fb_h = (uint32_t)fb->height, fb_pitch = (uint32_t)fb->pitch;
    struct vnode *v = vnode_alloc(VN_CHR, 0660, &fb_ops);
    KASSERT(v);
    v->rdev = (29 << 8) | 0;
    vfs_create_node("/dev/fb0", v);
    vnode_put(v);
    kprintf("fb: %lux%lu, %u bpp, /dev/fb0\n", fb->width, fb->height, fb->bpp);
}
