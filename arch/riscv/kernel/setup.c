#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/sched.h>
#include <linux/initrd.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/spi/xilinx_spi.h>

#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/pgtable.h>
#include <asm/smp.h>
#include <asm/sbi.h>
#include <asm/config-string.h>

static char __initdata command_line[COMMAND_LINE_SIZE];
#ifdef CONFIG_CMDLINE_BOOL
static char __initdata builtin_cmdline[COMMAND_LINE_SIZE] = CONFIG_CMDLINE;
#endif /* CONFIG_CMDLINE_BOOL */

unsigned long va_pa_offset;
unsigned long pfn_base;

#ifdef CONFIG_BLK_DEV_INITRD
static void __init setup_initrd(void)
{
	extern char __initramfs_start[];
	extern unsigned long __initramfs_size;
	unsigned long size;

	if (__initramfs_size > 0) {
		initrd_start = (unsigned long)(&__initramfs_start);
		initrd_end = initrd_start + __initramfs_size;
	}

	if (initrd_start >= initrd_end) {
		printk(KERN_INFO "initrd not found or empty");
		goto disable;
	}
	if (__pa(initrd_end) > PFN_PHYS(max_low_pfn)) {
		printk(KERN_ERR "initrd extends beyond end of memory");
		goto disable;
	}

	size =  initrd_end - initrd_start;
	memblock_reserve(__pa(initrd_start), size);
	initrd_below_start_ok = 1;

	printk(KERN_INFO "Initial ramdisk at: 0x%p (%lu bytes)\n",
		(void *)(initrd_start), size);
	return;
disable:
	printk(KERN_CONT " - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}
#endif /* CONFIG_BLK_DEV_INITRD */

static resource_size_t __initdata mem_size;

/* Parse "mem=nn[KkMmGg]" */
static int __init early_mem(char *p)
{
	if (!p)
		return -EINVAL;
	mem_size = memparse(p, &p) & PMD_MASK;
	if (mem_size == 0)
		return -EINVAL;
	return 0;
}
early_param("mem", early_mem);

static void __init reserve_boot_page_table(pte_t *table)
{
	unsigned long i;

	memblock_reserve(__pa(table), PAGE_SIZE);

	for (i = 0; i < PTRS_PER_PTE; i++) {
		if (pte_present(table[i]) && !pte_huge(table[i]))
			reserve_boot_page_table(pfn_to_virt(pte_pfn(table[i])));
	}
}

static void __init setup_bootmem(void)
{
	unsigned long ret;
	memory_block_info info;

	ret = sbi_query_memory(0, &info);
	BUG_ON(ret != 0);
	BUG_ON((info.base & ~PMD_MASK) != 0);
	BUG_ON((info.size & ~PMD_MASK) != 0);
	pr_info("Available physical memory: %ldMB\n", info.size >> 20);

	/* The kernel image is mapped at VA=PAGE_OFFSET and PA=info.base */
	va_pa_offset = PAGE_OFFSET - info.base;
	pfn_base = PFN_DOWN(info.base);

	if ((mem_size != 0) && (mem_size < info.size)) {
		memblock_enforce_memory_limit(mem_size);
		info.size = mem_size;
		pr_notice("Physical memory usage limited to %lluMB\n",
			(unsigned long long)(mem_size >> 20));
	}
	set_max_mapnr(PFN_DOWN(info.size));
	max_low_pfn = PFN_DOWN(info.base + info.size);

#ifdef CONFIG_BLK_DEV_INITRD
	setup_initrd();
#endif /* CONFIG_BLK_DEV_INITRD */

	memblock_reserve(info.base, __pa(_end) - info.base);
	reserve_boot_page_table(pfn_to_virt(csr_read(sptbr)));
	memblock_allow_resize();
}

// TODO: the following should all be handled by devicetree

static struct resource lowrisc_spi[] = {
	[0] = {
		.start = 0,
		.end   = 0xFF,
		.flags = IORESOURCE_MEM,
	},
};

static struct xspi_platform_data xspi_info = {
	.num_chipselect = 1,
	.bits_per_word = 8,
	.devices = NULL,
	.num_devices = 0,
};

static struct platform_device xspi_device = {
	.name = "xilinx_spi",
	.id = 0, /* Bus number */
	.num_resources = ARRAY_SIZE(lowrisc_spi),
	.resource = lowrisc_spi,
	.dev = {
		.platform_data = &xspi_info, /* Passed to driver */
	},
};

static struct spi_board_info lowrisc_spi_board_info[] __initdata = {
#if IS_ENABLED(CONFIG_MMC_SPI)
	{
		.modalias = "mmc_spi",
		.max_speed_hz = 25000000,     /* max spi clock (SCK) speed in HZ */
		.bus_num = 0,
		.chip_select = 0,
		.mode = SPI_MODE_0,
	},
#endif
};

static int __init lowrisc_setup_devinit(void)
{
	int ret;
	u64 spi_addr;

#if IS_ENABLED(CONFIG_SPI_XILINX)
	// Find config string driver
	struct device *csdev = bus_find_device_by_name(&platform_bus_type, NULL, "config-string");
	struct platform_device *pcsdev = to_platform_device(csdev);
	spi_addr = config_string_u64(pcsdev, "spi.addr");
	lowrisc_spi[0].start += spi_addr;
	lowrisc_spi[0].end += spi_addr;
	ret = platform_device_register(&xspi_device);

	spi_register_board_info(lowrisc_spi_board_info, ARRAY_SIZE(lowrisc_spi_board_info));
#endif

	return 0;
}

device_initcall(lowrisc_setup_devinit);


void __init setup_arch(char **cmdline_p)
{
#ifdef CONFIG_CMDLINE_BOOL
#ifdef CONFIG_CMDLINE_OVERRIDE
	strlcpy(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
#else
	if (builtin_cmdline[0] != '\0') {
		/* Append bootloader command line to built-in */
		strlcat(builtin_cmdline, " ", COMMAND_LINE_SIZE);
		strlcat(builtin_cmdline, boot_command_line, COMMAND_LINE_SIZE);
		strlcpy(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
	}
#endif /* CONFIG_CMDLINE_OVERRIDE */
#endif /* CONFIG_CMDLINE_BOOL */
	strlcpy(command_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = command_line;

	parse_early_param();

	init_mm.start_code = (unsigned long) _stext;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	init_mm.brk        = (unsigned long) _end;

	setup_bootmem();
#ifdef CONFIG_SMP
	setup_smp();
#endif
	paging_init();
}
