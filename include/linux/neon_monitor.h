// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Wimbi Yoas Hizkia <wimbiyoashizkia@yahoo.com>.
 */

#ifndef _NEON_MONITOR_H
#define _NEON_MONITOR_H

extern long si_mem_available(void);
extern bool memory_scan_monitor(void);
extern void memory_alloc_monitor(void);
extern void device_totalram(void);
extern unsigned long totalram_pages;
extern unsigned long zram_ram_usage;
extern unsigned long adreno_load;
extern int neon_swappiness;
extern int device_ram;
extern bool trigger_swap;

#endif /* _NEON_MONITOR_H */