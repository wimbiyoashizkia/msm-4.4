// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Wimbi Yoas Hizkia <wimbiyoashizkia@yahoo.com>.
 */

#include <asm/page.h>
#include <linux/neon_monitor.h>

int neon_swappiness = 60;

long total_memory;
void device_totalram(void)
{
	bool check_ram = false;
	int device_ram, threshold;

	if (!check_ram) {
		total_memory = totalram_pages << (PAGE_SHIFT - 10);

		if (total_memory > 5000000) {
			device_ram = 6; /* 6GB device */
			threshold = 15;
		} else if ((total_memory > 3000000) && (total_memory < 5000000)) {
			device_ram = 4; /* 4GB device */
			threshold = 20;
		} else {
			device_ram = 3; /* 3GB device */
			threshold = 40;
		}

		check_ram = true;
	}
}

void memory_monitor(void)
{
	long available_pages, available_memory;
	long available_memory_percent;

	/* Ram pages */
	available_pages = si_mem_available();
	available_memory = available_pages << (PAGE_SHIFT - 10);
	available_memory_percent = ((available_memory + zram_ram_usage) * 100) / total_memory;
}

bool memory_scan_monitor(void)
{
	bool vote = false;

	memory_monitor();
	vote = true;

	return vote;
}

void memory_alloc_monitor(void)
{
	bool trigger_swap = false;

	device_totalram();
	trigger_swap = memory_scan_monitor();
}
