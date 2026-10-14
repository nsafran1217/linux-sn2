/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1992 - 1997, 2000-2005 Silicon Graphics, Inc. All rights reserved.
 */
#ifndef _ASM_IA64_SN_PDA_TYPES_H
#define _ASM_IA64_SN_PDA_TYPES_H

/*
 * struct pda_s without the "pda" convenience macros of asm/sn/pda.h, so
 * that asm/mmiowb.h can reach the PIO write status fields without
 * defining "pda" in every file that takes a spinlock.
 */
#include <linux/types.h>
#include <asm/percpu.h>

/*
 * CPU-specific data structure.
 *
 * One of these structures is allocated for each cpu of a NUMA system.
 *
 * This structure provides a convenient way of keeping together 
 * all SN per-cpu data structures. 
 */

typedef struct pda_s {

	/*
	 * Support for SN LEDs
	 */
	volatile short	*led_address;
	u8		led_state;
	u8		hb_state;	/* supports blinking heartbeat leds */
	unsigned int	hb_count;

	unsigned int	idle_flag;
	
	volatile unsigned long *bedrock_rev_id;
	volatile unsigned long *pio_write_status_addr;
	unsigned long pio_write_status_val;
	volatile unsigned long *pio_shub_war_cam_addr;

	unsigned long	sn_in_service_ivecs[4];
	int		sn_lb_int_war_ticks;
	int		sn_last_irq;
	int		sn_first_irq;
} pda_t;

DECLARE_PER_CPU(struct pda_s, pda_percpu);

#endif /* _ASM_IA64_SN_PDA_TYPES_H */
