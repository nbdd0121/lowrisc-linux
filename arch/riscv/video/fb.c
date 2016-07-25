#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <linux/log2.h>

#include "vga.h"

#define VIDEOMEM_SIZE 0x400000
#define VIDEOMEM_ORDER (ilog2(VIDEOMEM_SIZE/PAGE_SIZE-1)+1)

// Method declarations
static int iris_probe(struct platform_device *dev);
static int iris_remove(struct platform_device *dev);
static int iris_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info);
static int iris_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
static int iris_set_par(struct fb_info *info);
static int iris_pan_display(struct fb_var_screeninfo *var, struct fb_info *info);

static struct platform_driver iris_driver = {
    .probe = iris_probe,
    .remove = iris_remove,
    .driver = {
        .name   = "fb_lowrisc",
        .owner  = THIS_MODULE,
    },
};

static struct platform_device *iris_device;

static struct fb_ops iris_ops = {
    .owner          = THIS_MODULE,
    .fb_check_var   = iris_check_var,
    .fb_set_par     = iris_set_par,
    .fb_setcolreg   = iris_setcolreg,
    .fb_pan_display = iris_pan_display,
};

struct iris_par {
    u32 pseudo_palette[16];
    u32 *reg;
};

static int iris_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *info) {
    if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
        u32 v;

        if (regno >= 16) {
            return -EINVAL;
        }

        v = (red << info->var.red.offset) |
            (green << info->var.green.offset) |
            (blue << info->var.blue.offset) |
            (transp << info->var.transp.offset);

        ((u32*)(info->pseudo_palette))[regno] = v;
    }

    return 0;
}

static int iris_check_var(struct fb_var_screeninfo *var, struct fb_info *info) {
    // Calculate depth from length & bpp
    int depth = var->red.length + var->green.length + var->blue.length;
    if (depth == 0 || abs(depth - var->bits_per_pixel) >= 8)
        depth = var->bits_per_pixel;

    int log2depth = ilog2(depth);
    int log2xres = ilog2(var->xres - 1) + 1;
    int log2bpl = log2xres + log2depth;

    // No enough video memory
    if (var->yres << log2bpl > VIDEOMEM_SIZE)
        return -EINVAL;

    // We use fixed pixel format, so adjust them
    switch (depth) {
        case 4:
            var->bits_per_pixel = 4;
            var->grayscale      = true;
            var->red.offset     = 0;
            var->red.length     = 0;
            var->green.offset   = 0;
            var->green.length   = 4;
            var->blue.offset    = 0;
            var->blue.length    = 0;
            var->transp.offset  = 0;
            var->transp.length  = 0;
            var->nonstd = true;
            break;
        case 8:
            var->bits_per_pixel = 8;
            var->grayscale      = false;
            var->red.offset     = 5;
            var->red.length     = 3;
            var->green.offset   = 2;
            var->green.length   = 3;
            var->blue.offset    = 0;
            var->blue.length    = 2;
            var->transp.offset  = 0;
            var->transp.length  = 0;
            var->nonstd         = false;
            break;
        case 16:
            var->bits_per_pixel = 16;
            var->grayscale      = false;
            var->red.offset     = 11;
            var->red.length     = 5;
            var->green.offset   = 5;
            var->green.length   = 6;
            var->blue.offset    = 0;
            var->blue.length    = 5;
            var->transp.offset  = 0;
            var->transp.length  = 0;
            var->nonstd         = false;
            break;
        case 32:
            var->bits_per_pixel = 32;
            var->grayscale      = false;
            var->red.offset     = 16;
            var->red.length     = 8;
            var->green.offset   = 8;
            var->green.length   = 8;
            var->blue.offset    = 0;
            var->blue.length    = 8;
            var->transp.offset  = 24;
            var->transp.length  = 8;
            var->nonstd         = false;
            break;
        default:
            return -EINVAL;
    }

    var->xoffset = 0;
    var->yoffset = 0;
    if (var->xres > 640) var->xres = 640;
    if (var->yres > 480) var->yres = 480;
    var->xres_virtual = 1 << log2xres;
    var->yres_virtual = VIDEOMEM_SIZE >> log2bpl;

    // dummy settings
    var->pixclock     = 10000000 / var->xres * 1000 / var->yres;
    var->left_margin  = (var->xres / 8) & 0xf8;
    var->hsync_len    = (var->xres / 8) & 0xf8;

    return 0;
}

static int iris_set_par(struct fb_info *info) {
    struct iris_par *par = info->par;
    int log2depth = ilog2(info->var.bits_per_pixel);

    iowrite32(0, par->reg + VIDEO_CR_ENABLE);
    iowrite32(5 - log2depth, par->reg + VIDEO_CR_DEPTH);
    iowrite32(info->var.xres, par->reg + VIDEO_CR_FB_WIDTH);
    iowrite32(info->var.yres, par->reg + VIDEO_CR_FB_HEIGHT);
    iowrite32(info->var.xres_virtual << log2depth, par->reg + VIDEO_CR_FB_BPL);
    iowrite32(1, par->reg + VIDEO_CR_ENABLE);

    info->fix.line_length = info->var.xres_virtual << log2depth;

    printk(KERN_INFO "lowRISC Iris: mode is %dx%dx%d, line length=%d", info->var.xres, info->var.yres, info->var.bits_per_pixel, info->fix.line_length);
    
    if (info->var.bits_per_pixel == 4) {
         printk(KERN_INFO "lowRISC Iris: grayscale: size=4\n");
    } else {
        printk(KERN_INFO "lowRISC Iris: truecolor: "
            "size=%d:%d:%d:%d, shift=%d:%d:%d:%d\n",
            info->var.transp.length,
            info->var.red.length,
            info->var.green.length,
            info->var.blue.length,
            info->var.transp.offset,
            info->var.red.offset,
            info->var.green.offset,
            info->var.blue.offset);
    }

    return 0;
}

static int iris_pan_display(struct fb_var_screeninfo *var, struct fb_info *info) {
    struct iris_par *par = info->par;
    uintptr_t mem = info->fix.smem_start;
    int log2bpl = ilog2(info->fix.line_length);
    int log2depth = ilog2(var->bits_per_pixel);
    mem += (var->yoffset << log2bpl) + (var->xoffset << log2depth);
    iowrite32((uint32_t)mem, par->reg + VIDEO_CR_BASE);
    iowrite32((uint32_t)(mem >> 32), par->reg + VIDEO_CR_BASE_HIGH);
    return 0;
}

static int iris_probe(struct platform_device *dev) {
    int ret = 0;

    struct fb_info *info;

    // Allocate contiguous memory as framebuffer
    unsigned char* mem = (unsigned char*)__get_free_pages(GFP_KERNEL, VIDEOMEM_ORDER);
    if (!mem) {
        printk(KERN_ERR "lowRISC Iris: failed to allocate video memory\n");
        return -ENOMEM;
    }
    memset(mem, 0, VIDEOMEM_SIZE);

    // Convert to bus address for DMA use
    phys_addr_t phymem = virt_to_phys(mem);

    // Request address
    if (!request_mem_region(0x40010000, SZ_4K, "lowrisc-iris")) {
        printk(KERN_WARNING
               "lowRISC Iris: cannot reserve video controller MMIO at 0x40010000\n");
    }

    // Allcoate framebuffer structure
    info = framebuffer_alloc(sizeof(struct iris_par), &dev->dev);
    if (!info) {
        printk(KERN_ERR "lowRISC Iris: failed to allocate framebuffer\n");
        free_pages((unsigned long)mem, VIDEOMEM_ORDER);
        return -ENOMEM;
    }

    info->fix = (struct fb_fix_screeninfo){
        .id             = "lowrisc-iris",
        .type           = FB_TYPE_PACKED_PIXELS,
        .visual         = FB_VISUAL_TRUECOLOR,
        .accel          = FB_ACCEL_NONE,
        .xpanstep       = 8,
        .ypanstep       = 1,
        .smem_start     = phymem,
        .smem_len       = VIDEOMEM_SIZE,
    };

    info->var = (struct fb_var_screeninfo){
        .activate       = FB_ACTIVATE_NOW,
        .vmode          = FB_VMODE_NONINTERLACED,
        .height         = -1,
        .width          = -1,
        .bits_per_pixel = 16,
        .xres           = 320,
        .yres           = 480,
    };

    // Setup mem location    
    info->fbops = &iris_ops;
    info->screen_base = mem;

    struct iris_par *par = info->par;
    platform_set_drvdata(dev, info);
    info->pseudo_palette = par;
    par->reg = ioremap_nocache(0x40010000, SZ_4K);

    info->flags = FBINFO_DEFAULT | FBINFO_VIRTFB;   
    info->apertures = NULL;

    // Write base registers
    iowrite32((uint32_t)(uintptr_t)phymem, par->reg + VIDEO_CR_BASE);
    iowrite32((uint32_t)((uintptr_t)phymem >> 32), par->reg + VIDEO_CR_BASE_HIGH);

    // Set mode (reuse some routines)
    iris_check_var(&info->var, info);
    iris_set_par(info);

    if (register_framebuffer(info) < 0) {
        printk(KERN_ERR "lowRISC Iris: Register framebuffer failed\n");
        return -EINVAL;
    }

    fb_info(info, "%s frame buffer device\n", info->fix.id);
    return ret;
}

static int iris_remove(struct platform_device *dev) {
    return 0;
}

static int __init iris_init(void) {
    printk(KERN_INFO "lowRISC Iris: initializing\n");
    int ret = platform_driver_register(&iris_driver);
    if (ret == 0) {
        iris_device = platform_device_alloc("fb_lowrisc", 0);
        if (iris_device) {
            ret = platform_device_add(iris_device);
        } else {
            ret = -ENOMEM;
        }
        if (ret != 0) {
            printk(KERN_INFO "lowRISC Iris: initialization failed\n");
            platform_device_put(iris_device);
            platform_driver_unregister(&iris_driver);
        } else {
            printk(KERN_INFO "lowRISC Iris: framebuffer initialized\n");
        }
    }
    return ret;
}

module_init(iris_init);

MODULE_LICENSE("BSD-2");
