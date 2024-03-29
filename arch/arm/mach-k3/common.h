/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * K3: Architecture common definitions
 *
 * Copyright (C) 2018 Texas Instruments Incorporated - http://www.ti.com/
 *	Lokesh Vutla <lokeshvutla@ti.com>
 */

#include <asm/armv7_mpu.h>

void setup_k3_mpu_regions(void);
int early_console_init(void);
void start_non_linux_remote_cores(void);
int load_firmware(char *name_fw, char *name_loadaddr, u32 *loadaddr);
