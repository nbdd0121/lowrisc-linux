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

static struct platform_driver iris_driver = {
	.probe = iris_probe,
	.remove = iris_remove,
	.driver = {
		.name	= "fb_lowrisc",
		.owner	= THIS_MODULE,
	},
};

static struct platform_device *iris_device;

static struct fb_fix_screeninfo iris_fix = {
    .id             = "lowrisc-iris",
    .type           = FB_TYPE_PACKED_PIXELS,
    .visual         = FB_VISUAL_TRUECOLOR,
    .accel          = FB_ACCEL_NONE,
};

static struct fb_var_screeninfo iris_var = {
    .activate       = FB_ACTIVATE_NOW,
    .vmode          = FB_VMODE_NONINTERLACED,
    .height		    = -1,
	.width	        = -1,
};

static struct fb_ops iris_ops = {
    .owner          = THIS_MODULE,
    .fb_setcolreg   = iris_setcolreg,
};

struct iris_par {
    u32 pseudo_palette[16];
    u32 *reg;
};

static int iris_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info) {
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

static int iris_probe(struct platform_device *dev) {
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

    // Setup iris_fix
    iris_fix.smem_start = phymem;
    iris_fix.smem_len = VIDEOMEM_SIZE;

    // Default mode
    iris_var.bits_per_pixel = 16;
    iris_var.xres = 320;
    iris_var.yres = 480;
    iris_fix.line_length = 1024;

    // Request address
    if (!request_mem_region(0x40010000, SZ_4K, "lowrisc-iris")) {
		printk(KERN_WARNING
		       "lowRISC Iris: cannot reserve video controller MMIO at 0x%lx\n", 0x40010000);
	}

    // Allcoate framebuffer structure
    info = framebuffer_alloc(sizeof(struct iris_par), &dev->dev);
    if (!info) {
        printk(KERN_ERR "lowRISC Iris: failed to allocate framebuffer\n");
        free_pages((unsigned long)mem, VIDEOMEM_ORDER);
        return -ENOMEM;
    }
    struct iris_par *par = info->par;
    platform_set_drvdata(dev, info);
    info->pseudo_palette = par;
    par->reg = ioremap_nocache(0x40010000, SZ_4K);

    info->apertures = NULL;

    printk(KERN_INFO "lowRISC Iris: mode is %dx%dx%d, linelength=%d", iris_var.xres, iris_var.yres, iris_var.bits_per_pixel, iris_fix.line_length);

    // Write control registers and enable display
    iowrite32((uint32_t)(uintptr_t)phymem, par->reg + VIDEO_CR_BASE);
    iowrite32((uint32_t)((uintptr_t)phymem >> 32), par->reg + VIDEO_CR_BASE_HIGH);
    iowrite32(1, par->reg + VIDEO_CR_DEPTH);
    iowrite32(320, par->reg + VIDEO_CR_FB_WIDTH);
    iowrite32(480, par->reg + VIDEO_CR_FB_HEIGHT);
    iowrite32(1024, par->reg + VIDEO_CR_FB_BPL);
    iowrite32(1, par->reg + VIDEO_CR_ENABLE);

    iris_var.xres_virtual = iris_var.xres;
    iris_var.yres_virtual = iris_fix.smem_len / iris_fix.line_length;

    iris_var.pixclock     = 10000000 / iris_var.xres * 1000 / iris_var.yres;
	iris_var.left_margin  = (iris_var.xres / 8) & 0xf8;
	iris_var.hsync_len    = (iris_var.xres / 8) & 0xf8;
	
	iris_var.red.offset    = 11;
	iris_var.red.length    = 5;
	iris_var.green.offset  = 5;
	iris_var.green.length  = 6;
	iris_var.blue.offset   = 0;
	iris_var.blue.length   = 5;
	iris_var.transp.offset = 0;
	iris_var.transp.length = 0;

    printk(KERN_INFO "lowRISC Iris: Truecolor: "
        "size=%d:%d:%d:%d, shift=%d:%d:%d:%d\n",
        iris_var.transp.length,
        iris_var.red.length,
        iris_var.green.length,
        iris_var.blue.length,
        iris_var.transp.offset,
        iris_var.red.offset,
        iris_var.green.offset,
        iris_var.blue.offset);

    info->screen_base = mem;
    if (!info->screen_base) {
        printk(KERN_ERR
            "lowRISC Iris: abort, cannot ioremap video memory 0x%x @ 0x%lx\n",
            mem, VIDEOMEM_SIZE);
		return -EIO;
    }

    info->fbops = &iris_ops;
    info->fix = iris_fix;
    info->var = iris_var;
    info->flags = FBINFO_DEFAULT | FBINFO_VIRTFB;    

    if (register_framebuffer(info) < 0) {
        printk(KERN_ERR "lowRISC Iris: Register framebuffer failed\n");
        return -EINVAL;
    }

    fb_info(info, "%s frame buffer device\n", info->fix.id);
    return 0;
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
