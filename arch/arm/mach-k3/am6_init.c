// SPDX-License-Identifier: GPL-2.0+
/*
 * K3: Architecture initialization
 *
 * Copyright (C) 2017-2018 Texas Instruments Incorporated - http://www.ti.com/
 *	Lokesh Vutla <lokeshvutla@ti.com>
 */

#include <common.h>
#include <asm/io.h>
#include <spl.h>
#include <asm/arch/hardware.h>
#include <asm/arch/sysfw-loader.h>
#include <asm/arch/sys_proto.h>
#include "common.h"
#include <dm.h>
#include <dm/uclass-internal.h>
#include <dm/pinctrl.h>
#include <linux/soc/ti/ti_sci_protocol.h>

extern void reinit_mmc_device(int dev);

#ifdef CONFIG_SPL_BUILD
#ifdef CONFIG_TI_SECURE_DEVICE
struct fwl_data {
	const char *name;
	u16 fwl_id;
	u16 regions;
} main_cbass_fwls[] = {
	{ "MMCSD1_CFG", 2057, 1 },
	{ "MMCSD0_CFG", 2058, 1 },
	{ "USB3SS0_SLV0", 2176, 2 },
	{ "PCIE0_SLV", 2336, 8 },
	{ "PCIE1_SLV", 2337, 8 },
	{ "PCIE0_CFG", 2688, 1 },
	{ "PCIE1_CFG", 2689, 1 },
}, mcu_cbass_fwls[] = {
	{ "MCU_ARMSS0_CORE0_SLV", 1024, 1 },
	{ "MCU_ARMSS0_CORE1_SLV", 1028, 1 },
	{ "MCU_FSS0_S1", 1033, 8 },
	{ "MCU_FSS0_S0", 1036, 8 },
	{ "MCU_CPSW0", 1220, 1 },
};

static void remove_fwl_configs(struct fwl_data *fwl_data, size_t fwl_data_size)
{
	struct ti_sci_msg_fwl_region region;
	struct ti_sci_fwl_ops *fwl_ops;
	struct ti_sci_handle *ti_sci;
	size_t i, j;

	ti_sci = get_ti_sci_handle();
	fwl_ops = &ti_sci->ops.fwl_ops;
	for (i = 0; i < fwl_data_size; i++) {
		for (j = 0; j <  fwl_data[i].regions; j++) {
			region.fwl_id = fwl_data[i].fwl_id;
			region.region = j;
			region.n_permission_regs = 3;

			fwl_ops->get_fwl_region(ti_sci, &region);

			if (region.control != 0) {
				pr_debug("Attempting to disable firewall %5d (%25s)\n",
					 region.fwl_id, fwl_data[i].name);
				region.control = 0;

				if (fwl_ops->set_fwl_region(ti_sci, &region))
					pr_err("Could not disable firewall %5d (%25s)\n",
					       region.fwl_id, fwl_data[i].name);
			}
		}
	}
}
#endif /* CONFIG_TI_SECURE_DEVICE */

static void mmr_unlock(u32 base, u32 partition)
{
	/* Translate the base address */
	phys_addr_t part_base = base + partition * CTRL_MMR0_PARTITION_SIZE;

	/* Unlock the requested partition if locked using two-step sequence */
	writel(CTRLMMR_LOCK_KICK0_UNLOCK_VAL, part_base + CTRLMMR_LOCK_KICK0);
	writel(CTRLMMR_LOCK_KICK1_UNLOCK_VAL, part_base + CTRLMMR_LOCK_KICK1);
}

static void ctrl_mmr_unlock(void)
{
	/* Unlock all WKUP_CTRL_MMR0 module registers */
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 0);
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 1);
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 2);
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 3);
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 6);
	mmr_unlock(WKUP_CTRL_MMR0_BASE, 7);

	/* Unlock all MCU_CTRL_MMR0 module registers */
	mmr_unlock(MCU_CTRL_MMR0_BASE, 0);
	mmr_unlock(MCU_CTRL_MMR0_BASE, 1);
	mmr_unlock(MCU_CTRL_MMR0_BASE, 2);
	mmr_unlock(MCU_CTRL_MMR0_BASE, 6);

	/* Unlock all CTRL_MMR0 module registers */
	mmr_unlock(CTRL_MMR0_BASE, 0);
	mmr_unlock(CTRL_MMR0_BASE, 1);
	mmr_unlock(CTRL_MMR0_BASE, 2);
	mmr_unlock(CTRL_MMR0_BASE, 3);
	mmr_unlock(CTRL_MMR0_BASE, 6);
	mmr_unlock(CTRL_MMR0_BASE, 7);
}

/*
 * This uninitialized global variable would normal end up in the .bss section,
 * but the .bss is cleared between writing and reading this variable, so move
 * it to the .data section.
 */
u32 bootindex __attribute__((section(".data")));

static void store_boot_index_from_rom(void)
{
	bootindex = *(u32 *)(CONFIG_SYS_K3_BOOT_PARAM_TABLE_INDEX);
}

void k3_init_post_pm_cb(void)
{
	if (spl_boot_device() == BOOT_DEVICE_MMC1)
		reinit_mmc_device(0);
	else if (spl_boot_device() == BOOT_DEVICE_MMC2)
		reinit_mmc_device(1);
}

static void setup_am654_navss_northbridge(void)
{
	/*
	 * NB0 is bridge to SRAM and NB1 is bridge to DDR.
	 * To ensure that SRAM transfers are not stalled due to
	 * delays during DDR refreshes, SRAM traffic should be higher
	 * priority (threadmap=2) than DDR traffic (threadmap=0).
	 */
	writel(0x2, NAVSS0_NBSS_NB0_CFG_BASE + NAVSS_NBSS_THREADMAP);
	writel(0x0, NAVSS0_NBSS_NB1_CFG_BASE + NAVSS_NBSS_THREADMAP);
}

void board_init_f(ulong dummy)
{
#if defined(CONFIG_K3_LOAD_SYSFW) || defined(CONFIG_K3_AM654_DDRSS)
	struct udevice *dev;
	int ret;
#endif
	/*
	 * Cannot delay this further as there is a chance that
	 * K3_BOOT_PARAM_TABLE_INDEX can be over written by SPL MALLOC section.
	 */
	store_boot_index_from_rom();

	/* Make all control module registers accessible */
	ctrl_mmr_unlock();

	setup_am654_navss_northbridge();

#ifdef CONFIG_CPU_V7R
	setup_k3_mpu_regions();

	/*
	 * When running SPL on R5 we are using SRAM for BSS to have global
	 * data etc. working prior to relocation. Since this means we need
	 * to self-manage BSS, clear that section now.
	 */
	memset(__bss_start, 0, __bss_end - __bss_start);
#endif

	/* Init DM early in-order to invoke system controller */
	spl_early_init();

#ifdef CONFIG_K3_EARLY_CONS
	/*
	 * Allow establishing an early console as required for example when
	 * doing a UART-based boot. Note that this console may not "survive"
	 * through a SYSFW PM-init step and will need a re-init in some way
	 * due to changing module clock frequencies.
	 */
	early_console_init();
#endif

#ifdef CONFIG_K3_LOAD_SYSFW
	/*
	 * Process pinctrl for the serial0 a.k.a. WKUP_UART0 module and continue
	 * regardless of the result of pinctrl. Do this without probing the
	 * device, but instead by searching the device that would request the
	 * given sequence number if probed. The UART will be used by the system
	 * firmware (SYSFW) image for various purposes and SYSFW depends on us
	 * to initialize its pin settings.
	 */
	ret = uclass_find_device_by_seq(UCLASS_SERIAL, 0, true, &dev);
	if (!ret) {
		pinctrl_select_state(dev, "default");
	}

	/*
	 * Load, start up, and configure system controller firmware while
	 * also populating the SYSFW post-PM configuration callback hook.
	 */
	k3_sysfw_loader(k3_init_post_pm_cb);
#endif

	/* Prepare console output */
	preloader_console_init();

#ifdef CONFIG_K3_LOAD_SYSFW
	/* Output System Firmware version info */
	k3_sysfw_loader_print_ver();
	/* Disable rom configured firewalls right after loading sysfw */
#ifdef CONFIG_TI_SECURE_DEVICE
	remove_fwl_configs(main_cbass_fwls, ARRAY_SIZE(main_cbass_fwls));
	remove_fwl_configs(mcu_cbass_fwls, ARRAY_SIZE(mcu_cbass_fwls));
#endif
#endif

	/* Perform EEPROM-based board detection */
	do_board_detect();

#if defined(CONFIG_CPU_V7R) && defined(CONFIG_K3_AVS0)
	ret = uclass_get_device(UCLASS_AVS, 0, &dev);
	if (ret)
		printf("AVS init failed: %d\n", ret);
#endif

#ifdef CONFIG_K3_AM654_DDRSS
	ret = uclass_get_device(UCLASS_RAM, 0, &dev);
	if (ret)
		panic("DRAM init failed: %d\n", ret);
#endif
}

u32 spl_boot_mode(const u32 boot_device)
{
#if defined(CONFIG_SUPPORT_EMMC_BOOT)
	u32 devstat = readl(CTRLMMR_MAIN_DEVSTAT);

	u32 bootmode = (devstat & CTRLMMR_MAIN_DEVSTAT_BOOTMODE_MASK) >>
			CTRLMMR_MAIN_DEVSTAT_BOOTMODE_SHIFT;

	/* eMMC boot0 mode is only supported for primary boot */
	if (bootindex == K3_PRIMARY_BOOTMODE &&
	    bootmode == BOOT_DEVICE_MMC1)
		return MMCSD_MODE_EMMCBOOT;
#endif

	/* Everything else use filesystem if available */
#if defined(CONFIG_SPL_FAT_SUPPORT) || defined(CONFIG_SPL_EXT_SUPPORT)
	return MMCSD_MODE_FS;
#else
	return MMCSD_MODE_RAW;
#endif
}

static u32 __get_backup_bootmedia(u32 devstat)
{
	u32 bkup_boot = (devstat & CTRLMMR_MAIN_DEVSTAT_BKUP_BOOTMODE_MASK) >>
			CTRLMMR_MAIN_DEVSTAT_BKUP_BOOTMODE_SHIFT;

	switch (bkup_boot) {
	case BACKUP_BOOT_DEVICE_USB:
		return BOOT_DEVICE_USB;
	case BACKUP_BOOT_DEVICE_UART:
		return BOOT_DEVICE_UART;
	case BACKUP_BOOT_DEVICE_ETHERNET:
		return BOOT_DEVICE_ETHERNET;
	case BACKUP_BOOT_DEVICE_MMC2:
	{
		u32 port = (devstat & CTRLMMR_MAIN_DEVSTAT_BKUP_MMC_PORT_MASK) >>
			    CTRLMMR_MAIN_DEVSTAT_BKUP_MMC_PORT_SHIFT;
		if (port == 0x0)
			return BOOT_DEVICE_MMC1;
		return BOOT_DEVICE_MMC2;
	}
	case BACKUP_BOOT_DEVICE_SPI:
		return BOOT_DEVICE_SPI;
	case BACKUP_BOOT_DEVICE_HYPERFLASH:
		return BOOT_DEVICE_HYPERFLASH;
	case BACKUP_BOOT_DEVICE_I2C:
		return BOOT_DEVICE_I2C;
	};

	return BOOT_DEVICE_RAM;
}

static u32 __get_primary_bootmedia(u32 devstat)
{
	u32 bootmode = (devstat & CTRLMMR_MAIN_DEVSTAT_BOOTMODE_MASK) >>
			CTRLMMR_MAIN_DEVSTAT_BOOTMODE_SHIFT;

	if (bootmode == BOOT_DEVICE_OSPI || bootmode ==	BOOT_DEVICE_QSPI)
		bootmode = BOOT_DEVICE_SPI;

	if (bootmode == BOOT_DEVICE_MMC2) {
		u32 port = (devstat & CTRLMMR_MAIN_DEVSTAT_MMC_PORT_MASK) >>
			    CTRLMMR_MAIN_DEVSTAT_MMC_PORT_SHIFT;
		if (port == 0x0)
			bootmode = BOOT_DEVICE_MMC1;
	} else if (bootmode == BOOT_DEVICE_MMC1) {
		u32 port = (devstat & CTRLMMR_MAIN_DEVSTAT_EMMC_PORT_MASK) >>
			    CTRLMMR_MAIN_DEVSTAT_EMMC_PORT_SHIFT;
		if (port == 0x1)
			bootmode = BOOT_DEVICE_MMC2;
	}

	return bootmode;
}

u32 spl_boot_device(void)
{
	u32 devstat = readl(CTRLMMR_MAIN_DEVSTAT);

	if (bootindex == K3_PRIMARY_BOOTMODE)
		return __get_primary_bootmedia(devstat);
	else
		return __get_backup_bootmedia(devstat);
}
#endif

#ifdef CONFIG_SYS_K3_SPL_ATF

#define AM6_DEV_MCU_RTI0			134
#define AM6_DEV_MCU_RTI1			135
#define AM6_DEV_MCU_ARMSS0_CPU0			159
#define AM6_DEV_MCU_ARMSS0_CPU1			245

void release_resources_for_core_shutdown(void)
{
	struct ti_sci_handle *ti_sci;
	struct ti_sci_dev_ops *dev_ops;
	struct ti_sci_proc_ops *proc_ops;
	int ret;
	u32 i;

	const u32 put_device_ids[] = {
		AM6_DEV_MCU_RTI0,
		AM6_DEV_MCU_RTI1,
	};

	ti_sci = get_ti_sci_handle();
	dev_ops = &ti_sci->ops.dev_ops;
	proc_ops = &ti_sci->ops.proc_ops;

	/* Iterate through list of devices to put (shutdown) */
	for (i = 0; i < ARRAY_SIZE(put_device_ids); i++) {
		u32 id = put_device_ids[i];

		ret = dev_ops->put_device(ti_sci, id);
		if (ret)
			panic("Failed to put device %u (%d)\n", id, ret);
	}

	const u32 put_core_ids[] = {
		AM6_DEV_MCU_ARMSS0_CPU1,
		AM6_DEV_MCU_ARMSS0_CPU0,	/* Handle CPU0 after CPU1 */
	};

	/* Iterate through list of cores to put (shutdown) */
	for (i = 0; i < ARRAY_SIZE(put_core_ids); i++) {
		u32 id = put_core_ids[i];

		/*
		 * Queue up the core shutdown request. Note that this call
		 * needs to be followed up by an actual invocation of an WFE
		 * or WFI CPU instruction.
		 */
		ret = proc_ops->proc_shutdown_no_wait(ti_sci, id);
		if (ret)
			panic("Failed sending core %u shutdown message (%d)\n",
			      id, ret);
	}
}
#endif
