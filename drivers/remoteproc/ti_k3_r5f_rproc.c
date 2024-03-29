// SPDX-License-Identifier: GPL-2.0+
/*
 * Texas Instruments' K3 R5 Remoteproc driver
 *
 * Copyright (C) 2018-2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Lokesh Vutla <lokeshvutla@ti.com>
 */

#include <common.h>
#include <dm.h>
#include <remoteproc.h>
#include <errno.h>
#include <clk.h>
#include <reset.h>
#include <asm/io.h>
#include <power-domain.h>
#include <linux/kernel.h>
#include <linux/soc/ti/ti_sci_protocol.h>
#include "ti_sci_proc.h"

/*
 * R5F's view of this address can either be for ATCM or BTCM with the other
 * at address 0x0 based on loczrama signal.
 */
#define K3_R5_TCM_DEV_ADDR	0x41010000

/* R5 TI-SCI Processor Configuration Flags */
#define PROC_BOOT_CFG_FLAG_R5_DBG_EN			0x00000001
#define PROC_BOOT_CFG_FLAG_R5_DBG_NIDEN			0x00000002
#define PROC_BOOT_CFG_FLAG_R5_LOCKSTEP			0x00000100
#define PROC_BOOT_CFG_FLAG_R5_TEINIT			0x00000200
#define PROC_BOOT_CFG_FLAG_R5_NMFI_EN			0x00000400
#define PROC_BOOT_CFG_FLAG_R5_TCM_RSTBASE		0x00000800
#define PROC_BOOT_CFG_FLAG_R5_BTCM_EN			0x00001000
#define PROC_BOOT_CFG_FLAG_R5_ATCM_EN			0x00002000
#define PROC_BOOT_CFG_FLAG_GEN_IGN_BOOTVECTOR		0x10000000

/* R5 TI-SCI Processor Control Flags */
#define PROC_BOOT_CTRL_FLAG_R5_CORE_HALT		0x00000001

/* R5 TI-SCI Processor Status Flags */
#define PROC_BOOT_STATUS_FLAG_R5_WFE			0x00000001
#define PROC_BOOT_STATUS_FLAG_R5_WFI			0x00000002
#define PROC_BOOT_STATUS_FLAG_R5_CLK_GATED		0x00000004
#define PROC_BOOT_STATUS_FLAG_R5_LOCKSTEP_PERMITTED	0x00000100

#define NR_CORES	2

enum cluster_mode {
	CLUSTER_MODE_SPLIT = 0,
	CLUSTER_MODE_LOCKSTEP,
};

/**
 * struct k3_r5_mem - internal memory structure
 * @cpu_addr: MPU virtual address of the memory region
 * @bus_addr: Bus address used to access the memory region
 * @dev_addr: Device address from remoteproc view
 * @size: Size of the memory region
 */
struct k3_r5f_mem {
	void __iomem *cpu_addr;
	phys_addr_t bus_addr;
	u32 dev_addr;
	size_t size;
};

/**
 * struct k3_r5f_core - K3 R5 core structure
 * @dev: cached device pointer
 * @cluster: pointer to the parent cluster.
 * @reset: reset control handle
 * pwrdmn: power domain handle
 * @tsp: TI-SCI processor control handle
 * @mem: Array of available internal memories
 * @num_mem: Number of available memories
 * @atcm_enable: flag to control ATCM enablement
 * @btcm_enable: flag to control BTCM enablement
 * @loczrama: flag to dictate which TCM is at device address 0x0
 * @in_use: flag to tell if the core is already in use.
 */
struct k3_r5f_core {
	struct udevice *dev;
	struct k3_r5f_cluster *cluster;
	struct reset_ctl reset;
	struct power_domain pwrdmn;
	struct ti_sci_proc tsp;
	struct k3_r5f_mem *mem;
	int num_mems;
	u32 atcm_enable;
	u32 btcm_enable;
	u32 loczrama;
	bool in_use;
};

/**
 * struct k3_r5f_cluster - K3 R5F Cluster structure
 * @mode: Mode to configure the Cluster - Split or LockStep
 * @cores: Array of pointers to R5 cores within the cluster
 */
struct k3_r5f_cluster {
	enum cluster_mode mode;
	struct k3_r5f_core *cores[NR_CORES];
};

static bool is_primary_core(struct k3_r5f_core *core)
{
	return core == core->cluster->cores[0];
}

static int k3_r5f_proc_request(struct k3_r5f_core *core)
{
	struct k3_r5f_cluster *cluster = core->cluster;
	int i, ret;

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP) {
		for (i = 0; i < NR_CORES; i++) {
			ret = ti_sci_proc_request(&cluster->cores[i]->tsp);
			if (ret)
				goto unroll_proc_request;
		}
	} else {
		ret = ti_sci_proc_request(&core->tsp);
	}

	return 0;

unroll_proc_request:
	while (i >= 0) {
		ti_sci_proc_release(&cluster->cores[i]->tsp);
		i--;
	}
	return ret;
}

static void k3_r5f_proc_release(struct k3_r5f_core *core)
{
	struct k3_r5f_cluster *cluster = core->cluster;
	int i;

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP)
		for (i = 0; i < NR_CORES; i++)
			ti_sci_proc_release(&cluster->cores[i]->tsp);
	else
		ti_sci_proc_release(&core->tsp);
}

static int k3_r5f_lockstep_release(struct k3_r5f_cluster *cluster)
{
	int ret, c;

	dev_dbg(dev, "%s\n", __func__);

	for (c = NR_CORES - 1; c >= 0; c--) {
		ret = power_domain_on(&cluster->cores[c]->pwrdmn);
		if (ret)
			goto unroll_module_reset;
	}

	/* deassert local reset on all applicable cores */
	for (c = NR_CORES - 1; c >= 0; c--) {
		ret = reset_deassert(&cluster->cores[c]->reset);
		if (ret)
			goto unroll_local_reset;
	}

	return 0;

unroll_local_reset:
	while (c < NR_CORES) {
		reset_assert(&cluster->cores[c]->reset);
		c++;
	}
	c = 0;
unroll_module_reset:
	while (c < NR_CORES) {
		power_domain_off(&cluster->cores[c]->pwrdmn);
		c++;
	}

	return ret;
}

static int k3_r5f_split_release(struct k3_r5f_core *core)
{
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = power_domain_on(&core->pwrdmn);
	if (ret) {
		dev_err(core->dev, "module-reset deassert failed, ret = %d\n",
			ret);
		return ret;
	}

	ret = reset_deassert(&core->reset);
	if (ret) {
		dev_err(core->dev, "local-reset deassert failed, ret = %d\n",
			ret);
		if (power_domain_off(&core->pwrdmn))
			dev_warn(core->dev, "module-reset assert back failed\n");
	}

	return ret;
}

static int k3_r5f_prepare(struct udevice *dev)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	struct k3_r5f_cluster *cluster = core->cluster;
	int ret = 0;

	dev_dbg(dev, "%s\n", __func__);

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP)
		ret = k3_r5f_lockstep_release(cluster);
	else
		ret = k3_r5f_split_release(core);

	if (ret)
		dev_err(dev, "Unable to enable cores for TCM loading %d\n",
			ret);

	return ret;
}

static int k3_r5f_core_sanity_check(struct k3_r5f_core *core)
{
	struct k3_r5f_cluster *cluster = core->cluster;

	if (core->in_use) {
		dev_err(dev, "Invalid op: Trying to load/start on already running core %d\n",
			core->tsp.proc_id);
		return -EINVAL;
	}

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP && !cluster->cores[1]) {
		printf("Secondary core is not probed in this cluster\n");
		return -EAGAIN;
	}

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP && !is_primary_core(core)) {
		dev_err(dev, "Invalid op: Trying to start secondary core %d in lockstep mode\n",
			core->tsp.proc_id);
		return -EINVAL;
	}

	if (cluster->mode == CLUSTER_MODE_SPLIT && !is_primary_core(core)) {
		if (!core->cluster->cores[0]->in_use) {
			dev_err(dev, "Invalid seq: Enable primary core before loading secondary core\n");
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * k3_r5f_load() - Load up the Remote processor image
 * @dev:	rproc device pointer
 * @addr:	Address at which image is available
 * @size:	size of the image
 *
 * Return: 0 if all goes good, else appropriate error message.
 */
static int k3_r5f_load(struct udevice *dev, ulong addr, ulong size)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	u32 boot_vector;
	int ret;

	dev_dbg(dev, "%s addr = 0x%lx, size = 0x%lx\n", __func__, addr, size);

	ret = k3_r5f_core_sanity_check(core);
	if (ret)
		return ret;

	ret = k3_r5f_proc_request(core);
	if (ret)
		return ret;

	ret = k3_r5f_prepare(dev);
	if (ret) {
		dev_err(dev, "R5f prepare failed for core %d\n",
			core->tsp.proc_id);
		return ret;
	}

	/* Zero out TCMs so that ECC can be effective on all TCM addresses */
	if (core->atcm_enable)
		memset(core->mem[0].cpu_addr, 0x00, core->mem[0].size);
	if (core->btcm_enable)
		memset(core->mem[1].cpu_addr, 0x00, core->mem[1].size);

	ret = rproc_elf_load_segments(dev, addr, size);
	if (ret < 0) {
		dev_err(dev, "Loading elf failedi %d\n", ret);
		return ret;
	}

	boot_vector = rproc_elf_get_boot_addr(dev, addr);

	dev_dbg(dev, "%s: Boot vector = 0x%x\n", __func__, boot_vector);

	ret = ti_sci_proc_set_config(&core->tsp, boot_vector, 0, 0);

	k3_r5f_proc_release(core);

	return ret;
}

static int k3_r5f_core_halt(struct k3_r5f_core *core)
{
	int ret;

	ret = ti_sci_proc_set_control(&core->tsp,
				      PROC_BOOT_CTRL_FLAG_R5_CORE_HALT, 0);
	if (ret)
		dev_err(core->dev, "Core %d failed to stop\n",
			core->tsp.proc_id);

	return ret;
}

static int k3_r5f_core_run(struct k3_r5f_core *core)
{
	int ret;

	ret = ti_sci_proc_set_control(&core->tsp,
				      0, PROC_BOOT_CTRL_FLAG_R5_CORE_HALT);
	if (ret) {
		dev_err(core->dev, "Core %d failed to start\n",
			core->tsp.proc_id);
		return ret;
	}

	return 0;
}

/**
 * k3_r5f_start() - Start the remote processor
 * @dev:	rproc device pointer
 *
 * Return: 0 if all went ok, else return appropriate error
 */
static int k3_r5f_start(struct udevice *dev)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	struct k3_r5f_cluster *cluster = core->cluster;
	int ret, c;

	dev_dbg(dev, "%s\n", __func__);

	ret = k3_r5f_core_sanity_check(core);
	if (ret)
		return ret;

	ret = k3_r5f_proc_request(core);
	if (ret)
		return ret;

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP) {
		if (is_primary_core(core)) {
			for (c = NR_CORES - 1; c >= 0; c--) {
				ret = k3_r5f_core_run(cluster->cores[c]);
				if (ret)
					goto unroll_core_run;
			}
		} else {
			dev_err(dev, "Invalid op: Trying to start secondary core %d in lockstep mode\n",
				core->tsp.proc_id);
			ret = -EINVAL;
			goto proc_release;
		}
	} else {
		ret = k3_r5f_core_run(core);
		if (ret)
			goto proc_release;
	}

	core->in_use = true;

	k3_r5f_proc_release(core);
	return 0;

unroll_core_run:
	while (c < NR_CORES) {
		k3_r5f_core_halt(cluster->cores[c]);
		c++;
	}
proc_release:
	k3_r5f_proc_release(core);

	return ret;
}

static int k3_r5f_split_reset(struct k3_r5f_core *core)
{
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	if (reset_assert(&core->reset))
		ret = -EINVAL;

	if (power_domain_off(&core->pwrdmn))
		ret = -EINVAL;

	return ret;
}

static int k3_r5f_lockstep_reset(struct k3_r5f_cluster *cluster)
{
	int ret = 0, c;

	dev_dbg(dev, "%s\n", __func__);

	for (c = 0; c < NR_CORES; c++)
		if (reset_assert(&cluster->cores[c]->reset))
			ret = -EINVAL;

	/* disable PSC modules on all applicable cores */
	for (c = 0; c < NR_CORES; c++)
		if (power_domain_off(&cluster->cores[c]->pwrdmn))
			ret = -EINVAL;

	return ret;
}

static int k3_r5f_unprepare(struct udevice *dev)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	struct k3_r5f_cluster *cluster = core->cluster;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP) {
		if (is_primary_core(core))
			ret = k3_r5f_lockstep_reset(cluster);
	} else {
		ret = k3_r5f_split_reset(core);
	}

	if (ret)
		dev_warn(dev, "Unable to enable cores for TCM loading %d\n",
			 ret);

	return 0;
}

static int k3_r5f_stop(struct udevice *dev)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	struct k3_r5f_cluster *cluster = core->cluster;
	int c, ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = k3_r5f_proc_request(core);
	if (ret)
		return ret;

	core->in_use = false;

	if (cluster->mode == CLUSTER_MODE_LOCKSTEP) {
		if (is_primary_core(core)) {
			for (c = 0; c < NR_CORES; c++)
				k3_r5f_core_halt(cluster->cores[c]);
		} else {
			dev_err(dev, "Invalid op: Trying to stop secondary core in lockstep mode\n");
			ret = -EINVAL;
			goto proc_release;
		}
	} else {
		k3_r5f_core_halt(core);
	}

	ret = k3_r5f_unprepare(dev);
proc_release:
	k3_r5f_proc_release(core);
	return ret;
}

static void *k3_r5f_da_to_va(struct udevice *dev, ulong da, ulong size)
{
	struct k3_r5f_core *core = dev_get_priv(dev);
	void __iomem *va = NULL;
	phys_addr_t bus_addr;
	u32 dev_addr, offset;
	ulong mem_size;
	int i;

	dev_dbg(dev, "%s\n", __func__);

	if (size <= 0)
		return NULL;

	for (i = 0; i < core->num_mems; i++) {
		bus_addr = core->mem[i].bus_addr;
		dev_addr = core->mem[i].dev_addr;
		mem_size = core->mem[i].size;

		if (da >= bus_addr && (da + size) <= (bus_addr + mem_size)) {
			offset = da - bus_addr;
			va = core->mem[i].cpu_addr + offset;
			return (__force void *)va;
		}

		if (da >= dev_addr && (da + size) <= (dev_addr + mem_size)) {
			offset = da - dev_addr;
			va = core->mem[i].cpu_addr + offset;
			return (__force void *)va;
		}
	}

	/* Assume it is DDR region and return da */
	return map_physmem(da, size, MAP_NOCACHE);
}

static int k3_r5f_init(struct udevice *dev)
{
	return 0;
}

static int k3_r5f_reset(struct udevice *dev)
{
	return 0;
}

static const struct dm_rproc_ops k3_r5f_rproc_ops = {
	.init = k3_r5f_init,
	.reset = k3_r5f_reset,
	.start = k3_r5f_start,
	.stop = k3_r5f_stop,
	.load = k3_r5f_load,
	.da_to_va = k3_r5f_da_to_va,
};

static int k3_r5f_rproc_configure(struct k3_r5f_core *core)
{
	struct k3_r5f_cluster *cluster = core->cluster;
	u32 set_cfg = 0, clr_cfg = 0, cfg, ctrl, sts;
	u64 boot_vec = 0;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = ti_sci_proc_request(&core->tsp);
	if (ret < 0)
		return ret;

	/* Do not touch boot vector now. Load will take care of it. */
	clr_cfg |= PROC_BOOT_CFG_FLAG_GEN_IGN_BOOTVECTOR;

	ret = ti_sci_proc_get_status(&core->tsp, &boot_vec, &cfg, &ctrl, &sts);
	if (ret)
		goto out;

	/* Sanity check for Lockstep mode */
	if (cluster->mode && is_primary_core(core) &&
	    !(sts & PROC_BOOT_STATUS_FLAG_R5_LOCKSTEP_PERMITTED)) {
		dev_err(core->dev, "LockStep mode not permitted on this device\n");
		ret = -EINVAL;
		goto out;
	}

	/* Primary core only configuration */
	if (is_primary_core(core)) {
		/* always enable ARM mode */
		clr_cfg |= PROC_BOOT_CFG_FLAG_R5_TEINIT;
		if (cluster->mode == CLUSTER_MODE_LOCKSTEP)
			set_cfg |= PROC_BOOT_CFG_FLAG_R5_LOCKSTEP;
		else
			clr_cfg |= PROC_BOOT_CFG_FLAG_R5_LOCKSTEP;
	}

	if (core->atcm_enable)
		set_cfg |= PROC_BOOT_CFG_FLAG_R5_ATCM_EN;
	else
		clr_cfg |= PROC_BOOT_CFG_FLAG_R5_ATCM_EN;

	if (core->btcm_enable)
		set_cfg |= PROC_BOOT_CFG_FLAG_R5_BTCM_EN;
	else
		clr_cfg |= PROC_BOOT_CFG_FLAG_R5_BTCM_EN;

	if (core->loczrama)
		set_cfg |= PROC_BOOT_CFG_FLAG_R5_TCM_RSTBASE;
	else
		clr_cfg |= PROC_BOOT_CFG_FLAG_R5_TCM_RSTBASE;

	ret = k3_r5f_core_halt(core);
	if (ret)
		goto out;

	ret = ti_sci_proc_set_config(&core->tsp, boot_vec,
				     set_cfg, clr_cfg);
out:
	ti_sci_proc_release(&core->tsp);
	return ret;
}

static int ti_sci_proc_of_to_priv(struct udevice *dev, struct ti_sci_proc *tsp)
{
	u32 ids[2];
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	tsp->sci = ti_sci_get_by_phandle(dev, "ti,sci");
	if (IS_ERR(tsp->sci)) {
		dev_err(dev, "ti_sci get failed: %ld\n", PTR_ERR(tsp->sci));
		return PTR_ERR(tsp->sci);
	}

	ret = dev_read_u32_array(dev, "ti,sci-proc-ids", ids, 2);
	if (ret) {
		dev_err(dev, "Proc IDs not populated %d\n", ret);
		return ret;
	}

	tsp->ops = &tsp->sci->ops.proc_ops;
	tsp->proc_id = ids[0];
	tsp->host_id = ids[1];

	return 0;
}

static int k3_r5f_of_to_priv(struct k3_r5f_core *core)
{
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	core->atcm_enable = dev_read_u32_default(core->dev, "atcm-enable", 0);
	core->btcm_enable = dev_read_u32_default(core->dev, "btcm-enable", 1);
	core->loczrama = dev_read_u32_default(core->dev, "loczrama", 1);

	ret = ti_sci_proc_of_to_priv(core->dev, &core->tsp);
	if (ret)
		return ret;

	ret = reset_get_by_index(core->dev, 0, &core->reset);
	if (ret) {
		dev_err(core->dev, "Reset lines not available: %d\n", ret);
		return ret;
	}

	ret = power_domain_get_by_index(core->dev, &core->pwrdmn, 0);
	if (ret) {
		dev_err(core->dev, "power_domain_get() failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int k3_r5f_core_of_get_memories(struct k3_r5f_core *core)
{
	static const char * const mem_names[] = {"atcm", "btcm"};
	int num_mems, internal_mems, reserved_mems;
	struct ofnode_phandle_args args;
	struct udevice *dev = core->dev;
	int i;

	dev_dbg(dev, "%s\n", __func__);

	internal_mems = ARRAY_SIZE(mem_names);
	reserved_mems = dev_count_phandle_with_args(dev, "memory-region", NULL);
	if (reserved_mems < 0)
		reserved_mems = 0;
	num_mems = internal_mems + reserved_mems;
	core->mem = calloc(num_mems, sizeof(*core->mem));
	if (!core->mem)
		return -ENOMEM;

	for (i = 0; i < internal_mems; i++) {
		core->mem[i].bus_addr =
		devfdt_get_addr_size_name(dev, mem_names[i],
					  (fdt_addr_t *)&core->mem[i].size);
		if (core->mem[i].bus_addr == FDT_ADDR_T_NONE) {
			dev_err(dev, "%s bus address not found\n",
				mem_names[i]);
			return -EINVAL;
		}
		core->mem[i].cpu_addr = map_physmem(core->mem[i].bus_addr,
						    core->mem[i].size,
						    MAP_NOCACHE);
		if (!strcmp(mem_names[i], "atcm")) {
			core->mem[i].dev_addr = core->loczrama ?
							0 : K3_R5_TCM_DEV_ADDR;
		} else {
			core->mem[i].dev_addr = core->loczrama ?
							K3_R5_TCM_DEV_ADDR : 0;
		}

		dev_dbg(dev, "memory %8s: bus addr %pa size 0x%zx va %p da 0x%x\n",
			mem_names[i], &core->mem[i].bus_addr,
			core->mem[i].size, core->mem[i].cpu_addr,
			core->mem[i].dev_addr);
	}

	for (i = internal_mems; i < num_mems; i++) {
		dev_read_phandle_with_args(dev, "memory-region", NULL, 0,
					   i - internal_mems, &args);
		core->mem[i].bus_addr =
			ofnode_get_addr_size(args.node, "reg",
					     (fdt_addr_t *)&core->mem[i].size);
		if (core->mem[i].bus_addr == FDT_ADDR_T_NONE) {
			dev_err(dev, "%s: Reserved address %d not found\n",
				__func__, i - internal_mems);
			return -EINVAL;
		}
		core->mem[i].cpu_addr = map_physmem(core->mem[i].bus_addr,
						    core->mem[i].size,
						    MAP_NOCACHE);
		/*
		 * Right now DDR map is same for R5 and Rest of the SoC.
		 * If MPU mapping is done, the address translation should
		 * come from image.
		 */
		core->mem[i].dev_addr = core->mem[i].bus_addr;

		dev_dbg(dev, "memory bus addr %pa size 0x%zx va %p da 0x%x\n",
			&core->mem[i].bus_addr, core->mem[i].size,
			core->mem[i].cpu_addr, core->mem[i].dev_addr);
	}
	core->num_mems = num_mems;

	return 0;
}

/**
 * k3_r5f_probe() - Basic probe
 * @dev:	corresponding k3 remote processor device
 *
 * Return: 0 if all goes good, else appropriate error message.
 */
static int k3_r5f_probe(struct udevice *dev)
{
	struct k3_r5f_cluster *cluster = dev_get_priv(dev->parent);
	struct k3_r5f_core *core = dev_get_priv(dev);
	bool r_state;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	core->dev = dev;
	ret = k3_r5f_of_to_priv(core);
	if (ret)
		return ret;

	core->cluster = cluster;
	/* Assume Primary core gets probed first */
	if (!cluster->cores[0])
		cluster->cores[0] = core;
	else
		cluster->cores[1] = core;

	ret = k3_r5f_core_of_get_memories(core);
	if (ret) {
		dev_err(dev, "Rproc getting internal memories failed\n");
		return ret;
	}

	ret = core->tsp.sci->ops.dev_ops.is_on(core->tsp.sci, core->pwrdmn.id,
					       &r_state, &core->in_use);
	if (ret)
		return ret;

	if (core->in_use) {
		dev_info(dev, "Core %d is already in use. No rproc commands work\n",
			 core->tsp.proc_id);
		return 0;
	}

	/* Make sure Local reset is asserted. Redundant? */
	reset_assert(&core->reset);

	ret = k3_r5f_rproc_configure(core);
	if (ret) {
		dev_err(dev, "rproc configure failed %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "Remoteproc successfully probed\n");

	return 0;
}

static int k3_r5f_remove(struct udevice *dev)
{
	struct k3_r5f_core *core = dev_get_priv(dev);

	free(core->mem);

	ti_sci_proc_release(&core->tsp);

	return 0;
}

static const struct udevice_id k3_r5f_rproc_ids[] = {
	{ .compatible = "ti,am654-r5f"},
	{ .compatible = "ti,j721e-r5f"},
	{}
};

U_BOOT_DRIVER(k3_r5f_rproc) = {
	.name = "k3_r5f_rproc",
	.of_match = k3_r5f_rproc_ids,
	.id = UCLASS_REMOTEPROC,
	.ops = &k3_r5f_rproc_ops,
	.probe = k3_r5f_probe,
	.remove = k3_r5f_remove,
	.priv_auto_alloc_size = sizeof(struct k3_r5f_core),
	.flags = DM_FLAG_DEFAULT_PD_EN_OFF,
};

static int k3_r5f_cluster_probe(struct udevice *dev)
{
	struct k3_r5f_cluster *cluster = dev_get_priv(dev);

	dev_dbg(dev, "%s\n", __func__);

	cluster->mode = dev_read_u32_default(dev, "lockstep-mode",
					     CLUSTER_MODE_LOCKSTEP);

	if (device_get_child_count(dev) != 2) {
		dev_err(dev, "Invalid number of R5 cores");
		return -EINVAL;
	}

	dev_dbg(dev, "%s: Cluster successfully probed in %s mode\n",
		__func__, cluster->mode ? "lockstep" : "split");

	return 0;
}

static const struct udevice_id k3_r5fss_ids[] = {
	{ .compatible = "ti,am654-r5fss"},
	{ .compatible = "ti,j721e-r5fss"},
	{}
};

U_BOOT_DRIVER(k3_r5fss) = {
	.name = "k3_r5fss",
	.of_match = k3_r5fss_ids,
	.id = UCLASS_MISC,
	.probe = k3_r5f_cluster_probe,
	.priv_auto_alloc_size = sizeof(struct k3_r5f_cluster),
	.bind = dm_scan_fdt_dev,
	.flags = DM_FLAG_DEFAULT_PD_EN_OFF,
};
