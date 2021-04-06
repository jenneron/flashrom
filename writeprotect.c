/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "spi.h"
#include "writeprotect.h"

/*
 * The following procedures rely on look-up tables to match the user-specified
 * range with the chip's supported ranges. This turned out to be the most
 * elegant approach since diferent flash chips use different levels of
 * granularity and methods to determine protected ranges. In other words,
 * be stupid and simple since clever arithmetic will not work for many chips.
 */

struct wp_range {
	unsigned int start;	/* starting address */
	unsigned int len;	/* len */
};

enum bit_state {
	OFF	= 0,
	ON	= 1,
	X	= -1	/* don't care. Must be bigger than max # of bp. */
};

/*
 * Generic write-protection schema for 25-series SPI flash chips. This assumes
 * there is a status register that contains one or more consecutive bits which
 * determine which address range is protected.
 */

struct status_register_layout {
	int bp0_pos;	/* position of BP0 */
	int bp_bits;	/* number of block protect bits */
	int srp_pos;	/* position of status register protect enable bit */
};

/*
 * The following ranges and functions are useful for representing the
 * writeprotect schema in which there are typically 5 bits of
 * relevant information stored in status register 1:
 * m.sec: This bit indicates the units (sectors vs. blocks)
 * m.tb: The top-bottom bit indicates if the affected range is at the top of
 *       the flash memory's address space or at the bottom.
 * bp: Bitmask representing the number of affected sectors/blocks.
 */
struct wp_range_descriptor {
	struct modifier_bits m;
	unsigned int bp;		/* block protect bitfield */
	struct wp_range range;
};

struct w25q_status {
	/* this maps to register layout -- do not change ordering */
	unsigned char busy : 1;
	unsigned char wel : 1;
	unsigned char bp0 : 1;
	unsigned char bp1 : 1;
	unsigned char bp2 : 1;
	unsigned char tb : 1;
	unsigned char sec : 1;
	unsigned char srp0 : 1;
} __attribute__ ((packed));

/* Status register for large flash layouts with 4 BP bits */
struct w25q_status_large {
	unsigned char busy : 1;
	unsigned char wel : 1;
	unsigned char bp0 : 1;
	unsigned char bp1 : 1;
	unsigned char bp2 : 1;
	unsigned char bp3 : 1;
	unsigned char tb : 1;
	unsigned char srp0 : 1;
} __attribute__ ((packed));

struct w25q_status_2 {
	unsigned char srp1 : 1;
	unsigned char qe : 1;
	unsigned char rsvd : 6;
} __attribute__ ((packed));

int w25_range_to_status(const struct flashctx *flash,
                        unsigned int start, unsigned int len,
                        struct w25q_status *status);
int w25_status_to_range(const struct flashctx *flash,
                        const struct w25q_status *status,
                        unsigned int *start, unsigned int *len);

/*
 * Mask to extract write-protect enable and range bits
 *   Status register 1:
 *     SRP0:           bit 7
 *     range(BP2-BP0): bit 4-2
 *     range(BP3-BP0): bit 5-2 (large chips)
 *   Status register 2:
 *     SRP1:           bit 1
 */
#define MASK_WP_AREA (0x9C)
#define MASK_WP_AREA_LARGE (0x9C)
#define MASK_WP2_AREA (0x01)

static struct wp_range_descriptor en25f40_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 504 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 496 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 480 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 448 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 384 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 512 * 1024} },
};

static struct wp_range_descriptor en25q40_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 504 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 496 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 480 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x0, {0x000000, 448 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 384 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 512 * 1024} },
};

static struct wp_range_descriptor en25q80_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 1016 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 1008 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 992 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 960 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 896 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 768 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 1024 * 1024} },
};

static struct wp_range_descriptor en25q32_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 4032 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 3968 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 3840 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 3584 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 3072 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 4096 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x010000, 4032 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x020000, 3968 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x040000, 3840 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x080000, 3584 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x100000, 3072 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x200000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 4096 * 1024} },
};

static struct wp_range_descriptor en25q64_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 8128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 8064 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 7936 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 7680 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 7168 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 6144 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 8192 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x010000, 8128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x020000, 8064 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x040000, 7936 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x080000, 7680 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x100000, 7168 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x200000, 6144 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 8192 * 1024} },
};

static struct wp_range_descriptor en25q128_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 16320 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 16256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 16128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 15872 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 15360 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 14336 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 16384 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x010000, 16320 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x020000, 16256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x040000, 16128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x080000, 15872 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x100000, 15360 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x200000, 14336 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 16384 * 1024} },
};

static struct wp_range_descriptor en25s64_ranges[] = {
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 8064 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 7936 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 7680 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 7168 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 6144 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 4096 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 8192 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x7e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x7c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x780000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x700000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x600000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x400000, 4096 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 8192 * 1024} },
};

/* mx25l1005 ranges also work for the mx25l1005c */
static struct wp_range_descriptor mx25l1005_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = X }, 0x1, {0x010000, 64 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x3, {0x000000, 128 * 1024} },
};

static struct wp_range_descriptor mx25l2005_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = X }, 0x1, {0x030000, 64 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x2, {0x020000, 128 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x3, {0x000000, 256 * 1024} },
};

static struct wp_range_descriptor mx25l4005_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = X }, 0x1, {0x070000, 64 * 1 * 1024} },	/* block 7 */
	{ .m = { .sec = X, .tb = X }, 0x2, {0x060000, 64 * 2 * 1024} },	/* blocks 6-7 */
	{ .m = { .sec = X, .tb = X }, 0x3, {0x040000, 64 * 4 * 1024} },	/* blocks 4-7 */
	{ .m = { .sec = X, .tb = X }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x5, {0x000000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x6, {0x000000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 512 * 1024} },
};

static struct wp_range_descriptor mx25l8005_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = X }, 0x1, {0x0f0000, 64 * 1 * 1024} },	/* block 15 */
	{ .m = { .sec = X, .tb = X }, 0x2, {0x0e0000, 64 * 2 * 1024} },	/* blocks 14-15 */
	{ .m = { .sec = X, .tb = X }, 0x3, {0x0c0000, 64 * 4 * 1024} },	/* blocks 12-15 */
	{ .m = { .sec = X, .tb = X }, 0x4, {0x080000, 64 * 8 * 1024} },	/* blocks 8-15 */
	{ .m = { .sec = X, .tb = X }, 0x5, {0x000000, 1024 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x6, {0x000000, 1024 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 1024 * 1024} },
};

static struct wp_range_descriptor mx25l1605d_ranges[] = {
	{ .m = { .sec = X, .tb = 0 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = 0 }, 0x1, {0x1f0000, 64 * 1 * 1024} },	/* block 31 */
	{ .m = { .sec = X, .tb = 0 }, 0x2, {0x1e0000, 64 * 2 * 1024} },	/* blocks 30-31 */
	{ .m = { .sec = X, .tb = 0 }, 0x3, {0x1c0000, 64 * 4 * 1024} },	/* blocks 28-31 */
	{ .m = { .sec = X, .tb = 0 }, 0x4, {0x180000, 64 * 8 * 1024} },	/* blocks 24-31 */
	{ .m = { .sec = X, .tb = 0 }, 0x5, {0x100000, 64 * 16 * 1024} },	/* blocks 16-31 */
	{ .m = { .sec = X, .tb = 0 }, 0x6, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
	{ .m = { .sec = X, .tb = 0 }, 0x7, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */

	{ .m = { .sec = X, .tb = 1 }, 0x0, {0x000000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x1, {0x000000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x2, {0x000000, 64 * 16 * 1024} },	/* blocks 0-15 */
	{ .m = { .sec = X, .tb = 1 }, 0x3, {0x000000, 64 * 24 * 1024} },	/* blocks 0-23 */
	{ .m = { .sec = X, .tb = 1 }, 0x4, {0x000000, 64 * 28 * 1024} },	/* blocks 0-27 */
	{ .m = { .sec = X, .tb = 1 }, 0x5, {0x000000, 64 * 30 * 1024} },	/* blocks 0-29 */
	{ .m = { .sec = X, .tb = 1 }, 0x6, {0x000000, 64 * 31 * 1024} },	/* blocks 0-30 */
	{ .m = { .sec = X, .tb = 1 }, 0x7, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
};

/* FIXME: Is there an mx25l3205 (without a trailing letter)? */
static struct wp_range_descriptor mx25l3205d_ranges[] = {
	{ .m = { .sec = X, .tb = 0 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = 0 }, 0x1, {0x3f0000, 64 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x2, {0x3e0000, 128 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x3, {0x3c0000, 256 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x4, {0x380000, 512 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x5, {0x300000, 1024 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x6, {0x200000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = 0 }, 0x7, {0x000000, 4096 * 1024} },

	{ .m = { .sec = X, .tb = 1 }, 0x0, {0x000000, 4096 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x1, {0x000000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x2, {0x000000, 3072 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x3, {0x000000, 3584 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x4, {0x000000, 3840 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x5, {0x000000, 3968 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x6, {0x000000, 4032 * 1024} },
	{ .m = { .sec = X, .tb = 1 }, 0x7, {0x000000, 4096 * 1024} },
};

static struct wp_range_descriptor mx25u3235e_ranges[] = {
	{ .m = { .sec = X, .tb = 0 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x3f0000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x3e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x3c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x380000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x300000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x200000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x000000, 4096 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x0, {0x000000, 4096 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 3072 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 3584 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 3840 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 3968 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 4032 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 4096 * 1024} },
};

static struct wp_range_descriptor mx25u6435e_ranges[] = {
	{ .m = { .sec = X, .tb = 0 }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x7f0000,   1 * 64 * 1024} },	/* block 127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x7e0000,   2 * 64 * 1024} },	/* blocks 126-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x7c0000,   4 * 64 * 1024} },	/* blocks 124-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x780000,   8 * 64 * 1024} },	/* blocks 120-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x700000,  16 * 64 * 1024} },	/* blocks 112-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x600000,  32 * 64 * 1024} },	/* blocks 96-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x400000,  64 * 64 * 1024} },	/* blocks 64-127 */

	{ .m = { .sec = 0, .tb = 1 }, 0x0, {0x000000,  64 * 64 * 1024} },	/* blocks 0-63 */
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000,  96 * 64 * 1024} },	/* blocks 0-95 */
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 112 * 64 * 1024} },	/* blocks 0-111 */
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 120 * 64 * 1024} },	/* blocks 0-119 */
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 124 * 64 * 1024} },	/* blocks 0-123 */
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 126 * 64 * 1024} },	/* blocks 0-125 */
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 127 * 64 * 1024} },	/* blocks 0-126 */
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000, 128 * 64 * 1024} },	/* blocks 0-127 */
};

#define MX25U12835E_TB	(1 << 3)
static struct wp_range_descriptor mx25u12835e_tb0_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0xff0000,   1 * 64 * 1024} },	/* block 255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0xfe0000,   2 * 64 * 1024} },	/* blocks 254-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0xfc0000,   4 * 64 * 1024} },	/* blocks 252-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0xf80000,   8 * 64 * 1024} },	/* blocks 248-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0xf00000,  16 * 64 * 1024} },	/* blocks 240-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0xe00000,  32 * 64 * 1024} },	/* blocks 224-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0xc00000,  64 * 64 * 1024} },	/* blocks 192-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x8, {0x800000,  128 * 64 * 1024} },	/* blocks 128-255 */
	{ .m = { .sec = 0, .tb = 0 }, 0x9, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xa, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xb, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xc, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xd, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xe, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 0 }, 0xf, {0x000000,  256 * 64 * 1024} },	/* blocks all */
};

static struct wp_range_descriptor mx25u12835e_tb1_ranges[] = {
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000,   1 * 64 * 1024} },	/* block 0 */
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000,   2 * 64 * 1024} },	/* blocks 0-1 */
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000,   4 * 64 * 1024} },	/* blocks 0-3 */
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000,   8 * 64 * 1024} },	/* blocks 0-7 */
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000,  16 * 64 * 1024} },	/* blocks 0-15 */
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000,  32 * 64 * 1024} },	/* blocks 0-31 */
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000,  64 * 64 * 1024} },	/* blocks 0-63 */
	{ .m = { .sec = 0, .tb = 1 }, 0x8, {0x000000,  128 * 64 * 1024} },	/* blocks 0-127 */
	{ .m = { .sec = 0, .tb = 1 }, 0x9, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xa, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xb, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xc, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xd, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xe, {0x000000,  256 * 64 * 1024} },	/* blocks all */
	{ .m = { .sec = 0, .tb = 1 }, 0xf, {0x000000,  256 * 64 * 1024} },	/* blocks all */
};

static struct wp_range_descriptor n25q064_ranges[] = {
	/*
	 * Note: For N25Q064, sec (usually in bit position 6) is called BP3
	 * (block protect bit 3). It is only useful when all blocks are to
	 * be write-protected.
	 */
	{ .m = { .sec = 0, .tb = 0 }, 0, {0, 0} },	/* none */

	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x7f0000,       64 * 1024} },	/* block 127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x7e0000,   2 * 64 * 1024} },	/* blocks 126-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x7c0000,   4 * 64 * 1024} },	/* blocks 124-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x780000,   8 * 64 * 1024} },	/* blocks 120-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x700000,  16 * 64 * 1024} },	/* blocks 112-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x600000,  32 * 64 * 1024} },	/* blocks 96-127 */
	{ .m = { .sec = 0, .tb = 0 }, 0x7, {0x400000,  64 * 64 * 1024} },	/* blocks 64-127 */

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000,       64 * 1024} },	/* block 0 */
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000,   2 * 64 * 1024} },	/* blocks 0-1 */
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000,   4 * 64 * 1024} },	/* blocks 0-3 */
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000,   8 * 64 * 1024} },	/* blocks 0-7 */
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000,  16 * 64 * 1024} },	/* blocks 0-15 */
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000,  32 * 64 * 1024} },	/* blocks 0-31 */
	{ .m = { .sec = 0, .tb = 1 }, 0x7, {0x000000,  64 * 64 * 1024} },	/* blocks 0-63 */

	{ .m = { .sec = X, .tb = 1 }, 0x0, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x1, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x2, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x3, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x4, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x5, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x6, {0x000000, 128 * 64 * 1024} },	/* all */
	{ .m = { .sec = X, .tb = 1 }, 0x7, {0x000000, 128 * 64 * 1024} },	/* all */
};

static struct wp_range_descriptor w25q16_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x1f0000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x1e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x1c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x180000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x100000, 1024 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 1024 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x6, {0x000000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 2048 * 1024} },

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x1ff000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x1fe000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x1fc000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x1f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x1f8000, 32 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor w25q32_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x3f0000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x3e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x3c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x380000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x300000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x200000, 2048 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 2048 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 4096 * 1024} },

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x3ff000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x3fe000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x3fc000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x3f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x3f8000, 32 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor w25q80_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x0f0000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x0e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x0c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x080000, 512 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x6, {0x000000, 1024 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 1024 * 1024} },

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x1ff000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x1fe000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x1fc000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x1f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x1f8000, 32 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor w25q64_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */

	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x7e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x7c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x780000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x700000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x600000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x400000, 4096 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 4096 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 8192 * 1024} },

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x7ff000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x7fe000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x7fc000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x7f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x7f8000, 32 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor w25rq128_cmp0_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },				/* NONE */

	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0xfc0000, 256 * 1024} },		/* Upper 1/64 */
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0xf80000, 512 * 1024} },		/* Upper 1/32 */
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0xf00000, 1024 * 1024} },		/* Upper 1/16 */
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0xe00000, 2048 * 1024} },		/* Upper 1/8 */
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0xc00000, 4096 * 1024} },		/* Upper 1/4 */
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x800000, 8192 * 1024} },		/* Upper 1/2 */

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 256 * 1024} },		/* Lower 1/64 */
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 512 * 1024} },		/* Lower 1/32 */
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 1024 * 1024} },		/* Lower 1/16 */
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 2048 * 1024} },		/* Lower 1/8 */
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 4096 * 1024} },		/* Lower 1/4 */
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 8192 * 1024} },		/* Lower 1/2 */

	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 16384 * 1024} },	/* ALL */

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0xfff000, 4 * 1024} },		/* Upper 1/4096 */
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0xffe000, 8 * 1024} },		/* Upper 1/2048 */
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0xffc000, 16 * 1024} },		/* Upper 1/1024 */
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0xff8000, 32 * 1024} },		/* Upper 1/512 */
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0xff8000, 32 * 1024} },		/* Upper 1/512 */

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },		/* Lower 1/4096 */
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },		/* Lower 1/2048 */
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },		/* Lower 1/1024 */
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },		/* Lower 1/512 */
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },		/* Lower 1/512 */
};

static struct wp_range_descriptor w25rq128_cmp1_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0x0, {0x000000, 16 * 1024 * 1024} },	/* ALL */

	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 16128 * 1024} },	/* Lower 63/64 */
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 15872 * 1024} },	/* Lower 31/32 */
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 15 * 1024 * 1024} },	/* Lower 15/16 */
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x000000, 14 * 1024 * 1024} },	/* Lower 7/8 */
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x000000, 12 * 1024 * 1024} },	/* Lower 3/4 */
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x000000, 8 * 1024 * 1024} },	/* Lower 1/2 */

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x040000, 16128 * 1024} },	/* Upper 63/64 */
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x080000, 15872 * 1024} },	/* Upper 31/32 */
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x100000, 15 * 1024 * 1024} },	/* Upper 15/16 */
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x200000, 14 * 1024 * 1024} },	/* Upper 7/8 */
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x400000, 12 * 1024 * 1024} },	/* Upper 3/4 */
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x800000, 8 * 1024 * 1024} },	/* Upper 1/2 */

	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 0} },			/* NONE */

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x000000, 16380 * 1024} },	/* Lower 4095/4096 */
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x000000, 16376 * 1024} },	/* Lower 2048/2048 */
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x000000, 16368 * 1024} },	/* Lower 1023/1024 */
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x000000, 16352 * 1024} },	/* Lower 511/512 */
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x000000, 16352 * 1024} },	/* Lower 511/512 */

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x001000, 16380 * 1024} },	/* Upper 4095/4096 */
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x002000, 16376 * 1024} },	/* Upper 2047/2048 */
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x004000, 16368 * 1024} },	/* Upper 1023/1024 */
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x008000, 16352 * 1024} },	/* Upper 511/512 */
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x008000, 16352 * 1024} },	/* Upper 511/512 */
};

static struct wp_range_descriptor w25rq256_cmp0_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0x0, {0x0000000, 0x0000000} },		/* NONE */

	{ .m = { .sec = X, .tb = 0 }, 0x1, {0x1ff0000, 64 * 1 * 1024} },	/* Upper 1/512 */
	{ .m = { .sec = X, .tb = 0 }, 0x2, {0x1fe0000, 64 * 2 * 1024} },	/* Upper 1/256 */
	{ .m = { .sec = X, .tb = 0 }, 0x3, {0x1fc0000, 64 * 4 * 1024} },	/* Upper 1/128 */
	{ .m = { .sec = X, .tb = 0 }, 0x4, {0x1f80000, 64 * 8 * 1024} },	/* Upper 1/64 */
	{ .m = { .sec = X, .tb = 0 }, 0x5, {0x1f00000, 64 * 16 * 1024} },	/* Upper 1/32 */
	{ .m = { .sec = X, .tb = 0 }, 0x6, {0x1e00000, 64 * 32 * 1024} },	/* Upper 1/16 */
	{ .m = { .sec = X, .tb = 0 }, 0x7, {0x1c00000, 64 * 64 * 1024} },	/* Upper 1/8 */
	{ .m = { .sec = X, .tb = 0 }, 0x8, {0x1800000, 64 * 128 * 1024} },	/* Upper 1/4 */
	{ .m = { .sec = X, .tb = 0 }, 0x9, {0x1000000, 64 * 256 * 1024} },	/* Upper 1/2 */

	{ .m = { .sec = X, .tb = 1 }, 0x1, {0x0000000, 64 * 1 * 1024} },	/* Lower 1/512 */
	{ .m = { .sec = X, .tb = 1 }, 0x2, {0x0000000, 64 * 2 * 1024} },	/* Lower 1/256 */
	{ .m = { .sec = X, .tb = 1 }, 0x3, {0x0000000, 64 * 4 * 1024} },	/* Lower 1/128 */
	{ .m = { .sec = X, .tb = 1 }, 0x4, {0x0000000, 64 * 8 * 1024} },	/* Lower 1/64 */
	{ .m = { .sec = X, .tb = 1 }, 0x5, {0x0000000, 64 * 16 * 1024} },	/* Lower 1/32 */
	{ .m = { .sec = X, .tb = 1 }, 0x6, {0x0000000, 64 * 32 * 1024} },	/* Lower 1/16 */
	{ .m = { .sec = X, .tb = 1 }, 0x7, {0x0000000, 64 * 64 * 1024} },	/* Lower 1/8 */
	{ .m = { .sec = X, .tb = 1 }, 0x8, {0x0000000, 64 * 128 * 1024} },	/* Lower 1/4 */
	{ .m = { .sec = X, .tb = 1 }, 0x9, {0x0000000, 64 * 256 * 1024} },	/* Lower 1/2 */

	{ .m = { .sec = X, .tb = X }, 0xa, {0x0000000, 64 * 512 * 1024} },	/* ALL */
	{ .m = { .sec = X, .tb = X }, 0xb, {0x0000000, 64 * 512 * 1024} },	/* ALL */
	{ .m = { .sec = X, .tb = X }, 0xc, {0x0000000, 64 * 512 * 1024} },	/* ALL */
	{ .m = { .sec = X, .tb = X }, 0xd, {0x0000000, 64 * 512 * 1024} },	/* ALL */
	{ .m = { .sec = X, .tb = X }, 0xe, {0x0000000, 64 * 512 * 1024} },	/* ALL */
	{ .m = { .sec = X, .tb = X }, 0xf, {0x0000000, 64 * 512 * 1024} },	/* ALL */
};

static struct wp_range_descriptor w25rq256_cmp1_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0x0, {0x0000000, 64 * 512 * 1024} },	/* ALL */

	{ .m = { .sec = X, .tb = 0 }, 0x1, {0x0000000, 64 * 511 * 1024} },	/* Lower 511/512 */
	{ .m = { .sec = X, .tb = 0 }, 0x2, {0x0000000, 64 * 510 * 1024} },	/* Lower 255/256 */
	{ .m = { .sec = X, .tb = 0 }, 0x3, {0x0000000, 64 * 508 * 1024} },	/* Lower 127/128 */
	{ .m = { .sec = X, .tb = 0 }, 0x4, {0x0000000, 64 * 504 * 1024} },	/* Lower 63/64 */
	{ .m = { .sec = X, .tb = 0 }, 0x5, {0x0000000, 64 * 496 * 1024} },	/* Lower 31/32 */
	{ .m = { .sec = X, .tb = 0 }, 0x6, {0x0000000, 64 * 480 * 1024} },	/* Lower 15/16 */
	{ .m = { .sec = X, .tb = 0 }, 0x7, {0x0000000, 64 * 448 * 1024} },	/* Lower 7/8 */
	{ .m = { .sec = X, .tb = 0 }, 0x8, {0x0000000, 64 * 384 * 1024} },	/* Lower 3/4 */
	{ .m = { .sec = X, .tb = 0 }, 0x9, {0x0000000, 64 * 256 * 1024} },	/* Lower 1/2 */

	{ .m = { .sec = X, .tb = 1 }, 0x1, {0x0010000, 64 * 511 * 1024} },	/* Upper 511/512 */
	{ .m = { .sec = X, .tb = 1 }, 0x2, {0x0020000, 64 * 510 * 1024} },	/* Upper 255/256 */
	{ .m = { .sec = X, .tb = 1 }, 0x3, {0x0040000, 64 * 508 * 1024} },	/* Upper 127/128 */
	{ .m = { .sec = X, .tb = 1 }, 0x4, {0x0080000, 64 * 504 * 1024} },	/* Upper 63/64 */
	{ .m = { .sec = X, .tb = 1 }, 0x5, {0x0100000, 64 * 496 * 1024} },	/* Upper 31/32 */
	{ .m = { .sec = X, .tb = 1 }, 0x6, {0x0200000, 64 * 480 * 1024} },	/* Upper 15/16 */
	{ .m = { .sec = X, .tb = 1 }, 0x7, {0x0400000, 64 * 448 * 1024} },	/* Upper 7/8 */
	{ .m = { .sec = X, .tb = 1 }, 0x8, {0x0800000, 64 * 384 * 1024} },	/* Upper 3/4 */
	{ .m = { .sec = X, .tb = 1 }, 0x9, {0x1000000, 64 * 256 * 1024} },	/* Upper 1/2 */

	{ .m = { .sec = X, .tb = X }, 0xa, {0x0000000, 0x0000000} },		/* NONE */
	{ .m = { .sec = X, .tb = X }, 0xb, {0x0000000, 0x0000000} },		/* NONE */
	{ .m = { .sec = X, .tb = X }, 0xc, {0x0000000, 0x0000000} },		/* NONE */
	{ .m = { .sec = X, .tb = X }, 0xd, {0x0000000, 0x0000000} },		/* NONE */
	{ .m = { .sec = X, .tb = X }, 0xe, {0x0000000, 0x0000000} },		/* NONE */
	{ .m = { .sec = X, .tb = X }, 0xf, {0x0000000, 0x0000000} },		/* NONE */
};

static struct wp_range_descriptor w25x10_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x010000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x3, {0x000000, 128 * 1024} },
};

static struct wp_range_descriptor w25x20_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x030000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x020000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x3, {0x000000, 256 * 1024} },
};

static struct wp_range_descriptor w25x40_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x070000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x060000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x040000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x5, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x6, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x7, {0x000000, 512 * 1024} },
};

static struct wp_range_descriptor w25x80_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },    /* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x0F0000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x0E0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x0C0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x080000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x5, {0x000000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x6, {0x000000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x7, {0x000000, 1024 * 1024} },
};

static struct wp_range_descriptor gd25q40_cmp0_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },    /* None */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x070000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x060000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x040000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 64 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = X }, 0x4, {0x000000, 512 * 1024} }, /* All */
	{ .m = { .sec = 0, .tb = X }, 0x5, {0x000000, 512 * 1024} }, /* All */
	{ .m = { .sec = 0, .tb = X }, 0x6, {0x000000, 512 * 1024} }, /* All */
	{ .m = { .sec = 0, .tb = X }, 0x7, {0x000000, 512 * 1024} }, /* All */
	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x07F000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x07E000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x07C000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x078000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x078000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x6, {0x078000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x6, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = X }, 0x7, {0x000000, 512 * 1024} }, /* All */
};

static struct wp_range_descriptor gd25q40_cmp1_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0x0, {0x000000, 512 * 1024} }, /* ALL */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x000000, 448 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x000000, 384 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x000000, 256 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x010000, 448 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x020000, 384 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x040000, 256 * 1024} },

	{ .m = { .sec = 0, .tb = X }, 0x4, {0x000000, 0} }, /* None */
	{ .m = { .sec = 0, .tb = X }, 0x5, {0x000000, 0} }, /* None */
	{ .m = { .sec = 0, .tb = X }, 0x6, {0x000000, 0} }, /* None */
	{ .m = { .sec = 0, .tb = X }, 0x7, {0x000000, 0} }, /* None */

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x000000, 508 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x000000, 504 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x000000, 496 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x000000, 480 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x000000, 480 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x6, {0x000000, 480 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x001000, 508 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x002000, 504 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x004000, 496 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x008000, 480 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x008000, 480 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x6, {0x008000, 480 * 1024} },

	{ .m = { .sec = 1, .tb = X }, 0x7, {0x000000, 0} }, /* None */
};

static struct wp_range_descriptor gd25q64_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0, {0, 0} },	/* none */
	{ .m = { .sec = 0, .tb = 0 }, 0x1, {0x7e0000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x2, {0x7c0000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x3, {0x780000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x4, {0x700000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x5, {0x600000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 0 }, 0x6, {0x400000, 4096 * 1024} },

	{ .m = { .sec = 0, .tb = 1 }, 0x1, {0x000000, 128 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x2, {0x000000, 256 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x3, {0x000000, 512 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x4, {0x000000, 1024 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x5, {0x000000, 2048 * 1024} },
	{ .m = { .sec = 0, .tb = 1 }, 0x6, {0x000000, 4096 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x000000, 8192 * 1024} },

	{ .m = { .sec = 1, .tb = 0 }, 0x1, {0x7ff000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x2, {0x7fe000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x3, {0x7fc000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x4, {0x7f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x5, {0x7f8000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 0 }, 0x6, {0x7f8000, 32 * 1024} },

	{ .m = { .sec = 1, .tb = 1 }, 0x1, {0x000000, 4 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x2, {0x000000, 8 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x3, {0x000000, 16 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x4, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x5, {0x000000, 32 * 1024} },
	{ .m = { .sec = 1, .tb = 1 }, 0x6, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor a25l040_ranges[] = {
	{ .m = { .sec = X, .tb = X }, 0x0, {0, 0} },	/* none */
	{ .m = { .sec = X, .tb = X }, 0x1, {0x70000, 64 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x2, {0x60000, 128 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x3, {0x40000, 256 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x4, {0x00000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x5, {0x00000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x6, {0x00000, 512 * 1024} },
	{ .m = { .sec = X, .tb = X }, 0x7, {0x00000, 512 * 1024} },
};

struct wp *get_wp_for_flashchip(const struct flashchip *chip) {
	// FIXME: The .wp field should be deleted from from struct flashchip
	// completly, but linux_mtd and cros_ec still assign their own values
	// to it. When they are cleaned up we can delete this.
	if(chip->wp) return chip->wp;

	switch (chip->manufacture_id) {
	case WINBOND_NEX_ID:
		switch(chip->model_id) {
		case WINBOND_NEX_W25X10:
		case WINBOND_NEX_W25X20:
		case WINBOND_NEX_W25X40:
		case WINBOND_NEX_W25X80:
		case WINBOND_NEX_W25Q128_V_M:
			return &wp_w25;
		case WINBOND_NEX_W25Q80_V:
		case WINBOND_NEX_W25Q16_V:
		case WINBOND_NEX_W25Q32_V:
		case WINBOND_NEX_W25Q32_W:
		case WINBOND_NEX_W25Q32JW:
		case WINBOND_NEX_W25Q64_V:
                case WINBOND_NEX_W25Q64_W:
		// W25Q64JW does not have a range table entry, but the flashchip
		// set .wp to wp_25q, so keep it here until the issue is resolved
		case WINBOND_NEX_W25Q64JW:
		case WINBOND_NEX_W25Q128_DTR:
		case WINBOND_NEX_W25Q128_V:
		case WINBOND_NEX_W25Q128_W:
			return &wp_w25q;
		case WINBOND_NEX_W25Q256_V:
		case WINBOND_NEX_W25Q256JV_M:
			return &wp_w25q_large;
		}
		break;
	case EON_ID_NOPREFIX:
		switch (chip->model_id) {
		case EON_EN25F40:
		case EON_EN25Q40:
		case EON_EN25Q80:
		case EON_EN25Q32:
		case EON_EN25Q64:
		case EON_EN25Q128:
		case EON_EN25QH128:
		case EON_EN25S64:
			return &wp_w25;
		}
		break;
	case MACRONIX_ID:
		switch (chip->model_id) {
		case MACRONIX_MX25L1005:
		case MACRONIX_MX25L2005:
		case MACRONIX_MX25L4005:
		case MACRONIX_MX25L8005:
		case MACRONIX_MX25L1605:
		case MACRONIX_MX25L3205:
		case MACRONIX_MX25U3235E:
		case MACRONIX_MX25U6435E:
			return &wp_w25;
		case MACRONIX_MX25U12835E:
			return &wp_w25q_large;
		case MACRONIX_MX25L6405:
		case MACRONIX_MX25L6495F:
		case MACRONIX_MX25L25635F:
			return &wp_generic;
		}
		break;
	case ST_ID:
		switch(chip->model_id) {
		case ST_N25Q064__1E:
		case ST_N25Q064__3E:
			return &wp_w25;
		}
		break;
	case GIGADEVICE_ID:
		switch(chip->model_id) {
		case GIGADEVICE_GD25LQ32:
		// GD25Q40 does not have a .wp field in flashchips.c, but
		// it is in the w25 range table function, so note it here
		// until the issue is resolved:
		// case GIGADEVICE_GD25Q40:
		case GIGADEVICE_GD25Q64:
		case GIGADEVICE_GD25LQ64:
		case GIGADEVICE_GD25Q128:
			return &wp_w25;
		case GIGADEVICE_GD25Q256D:
			return &wp_w25q_large;
		case GIGADEVICE_GD25LQ128CD:
		case GIGADEVICE_GD25Q32:
			return &wp_generic;
		}
		break;
	case AMIC_ID_NOPREFIX:
		switch(chip->model_id) {
		case AMIC_A25L040:
			return &wp_w25;
		}
		break;
	case ATMEL_ID:
		switch(chip->model_id) {
		case ATMEL_AT25SF128A:
		case ATMEL_AT25SL128A:
			return &wp_w25q;
		}
		break;
	case PROGMANUF_ID:
		switch(chip->model_id) {
		case PROGDEV_ID:
			return &wp_w25;
		}
		break;
	case SPANSION_ID:
		switch (chip->model_id) {
		case SPANSION_S25FS128S_L:
		case SPANSION_S25FS128S_S:
		case SPANSION_S25FL256S_UL:
		case SPANSION_S25FL256S_US:
		// SPANSION_S25FL128S_UL does not have a range table entry,
		// but its flashchip set .wp to wp_generic, so keep it here
		// until the issue resolved
		case SPANSION_S25FL128S_UL:
		// SPANSION_S25FL128S_US does not have a range table entry,
		// but its flashchip set .wp to wp_generic, so keep it here
		// until the issue resolved
		case SPANSION_S25FL128S_US:
			return &wp_generic;
		}
		break;
	}


	return NULL;
}

/* FIXME: Move to spi25.c if it's a JEDEC standard opcode */
static uint8_t w25q_read_status_register_2(const struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_RDSR_OUTSIZE] = { 0x35 };
	unsigned char readarr[2];
	int ret;

	if (flash->chip->read_status) {
		msg_cdbg("RDSR2 failed! cmd=0x35 unimpl for opaque chips\n");
		return 0;
	}

	/* Read Status Register */
	ret = spi_send_command(flash, sizeof(cmd), sizeof(readarr), cmd, readarr);
	if (ret) {
		/*
		 * FIXME: make this a benign failure for now in case we are
		 * unable to execute the opcode
		 */
		msg_cdbg("RDSR2 failed!\n");
		readarr[0] = 0x00;
	}

	return readarr[0];
}

/* FIXME: Move to spi25.c if it's a JEDEC standard opcode */
static uint8_t mx25l_read_config_register(const struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_RDSR_OUTSIZE] = { 0x15 };
	unsigned char readarr[2];	/* leave room for dummy byte */
	int ret;

	if (flash->chip->read_status) {
		msg_cdbg("RDCR failed! cmd=0x15 unimpl for opaque chips\n");
		return 0;
	}

	ret = spi_send_command(flash, sizeof(cmd), sizeof(readarr), cmd, readarr);
	if (ret) {
		msg_cdbg("RDCR failed!\n");
		readarr[0] = 0x00;
	}

	return readarr[0];
}

static int generic_range_table(const struct flashctx *flash,
                           struct wp_range_descriptor **descrs,
                           int *num_entries);

/* Given a flash chip, this function returns its range table. */
static int w25_range_table(const struct flashctx *flash,
                           struct wp_range_descriptor **descrs,
                           int *num_entries)
{
	*descrs = 0;
	*num_entries = 0;

	switch (flash->chip->manufacture_id) {
	case WINBOND_NEX_ID:
	case EON_ID_NOPREFIX:
	case MACRONIX_ID:
		return generic_range_table(flash, descrs, num_entries);
	case ST_ID:
		switch(flash->chip->model_id) {
		case ST_N25Q064__1E:
		case ST_N25Q064__3E:
			*descrs = n25q064_ranges;
			*num_entries = ARRAY_SIZE(n25q064_ranges);
			break;
		default:
			msg_cerr("%s() %d: Micron flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;
	case GIGADEVICE_ID:
		switch(flash->chip->model_id) {
		case GIGADEVICE_GD25LQ32:
			*descrs = w25q32_ranges;
			*num_entries = ARRAY_SIZE(w25q32_ranges);
			break;
		case GIGADEVICE_GD25Q40:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = gd25q40_cmp1_ranges;
				*num_entries = ARRAY_SIZE(gd25q40_cmp1_ranges);
			} else {
				*descrs = gd25q40_cmp0_ranges;
				*num_entries = ARRAY_SIZE(gd25q40_cmp0_ranges);
			}
			break;
		case GIGADEVICE_GD25Q64:
		case GIGADEVICE_GD25LQ64:
			*descrs = gd25q64_ranges;
			*num_entries = ARRAY_SIZE(gd25q64_ranges);
			break;
		case GIGADEVICE_GD25Q128:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = w25rq128_cmp1_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp1_ranges);
			} else {
				/* CMP == 0 */
				*descrs = w25rq128_cmp0_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp0_ranges);
			}
			break;
		case GIGADEVICE_GD25Q256D:
			*descrs = w25rq256_cmp0_ranges;
			*num_entries = ARRAY_SIZE(w25rq256_cmp0_ranges);
			break;
		default:
			msg_cerr("%s() %d: GigaDevice flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;
	case AMIC_ID_NOPREFIX:
		switch(flash->chip->model_id) {
		case AMIC_A25L040:
			*descrs = a25l040_ranges;
			*num_entries = ARRAY_SIZE(a25l040_ranges);
			break;
		default:
			msg_cerr("%s() %d: AMIC flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;
	case ATMEL_ID:
		switch(flash->chip->model_id) {
		case ATMEL_AT25SF128A:
		case ATMEL_AT25SL128A:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = w25rq128_cmp1_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp1_ranges);
			} else {
				/* CMP == 0 */
				*descrs = w25rq128_cmp0_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp0_ranges);
			}
			break;
		default:
			msg_cerr("%s() %d: Atmel flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;
	default:
		msg_cerr("%s: flash vendor (0x%x) not found, aborting\n",
		         __func__, flash->chip->manufacture_id);
		return -1;
	}

	return 0;
}

int w25_range_to_status(const struct flashctx *flash,
                        unsigned int start, unsigned int len,
                        struct w25q_status *status)
{
	struct wp_range_descriptor *descrs;
	int i, range_found = 0;
	int num_entries;

	if (w25_range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		struct wp_range *r = &descrs[i].range;

		msg_cspew("comparing range 0x%x 0x%x / 0x%x 0x%x\n",
			  start, len, r->start, r->len);
		if ((start == r->start) && (len == r->len)) {
			status->bp0 = descrs[i].bp & 1;
			status->bp1 = descrs[i].bp >> 1;
			status->bp2 = descrs[i].bp >> 2;
			status->tb = descrs[i].m.tb;
			status->sec = descrs[i].m.sec;

			range_found = 1;
			break;
		}
	}

	if (!range_found) {
		msg_cerr("%s: matching range not found\n", __func__);
		return -1;
	}

	return 0;
}

int w25_status_to_range(const struct flashctx *flash,
                        const struct w25q_status *status,
                        unsigned int *start, unsigned int *len)
{
	struct wp_range_descriptor *descrs;
	int i, status_found = 0;
	int num_entries;

	if (w25_range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		int bp;
		int table_bp, table_tb, table_sec;

		bp = status->bp0 | (status->bp1 << 1) | (status->bp2 << 2);
		msg_cspew("comparing  0x%x 0x%x / 0x%x 0x%x / 0x%x 0x%x\n",
		          bp, descrs[i].bp,
		          status->tb, descrs[i].m.tb,
		          status->sec, descrs[i].m.sec);
		table_bp = descrs[i].bp;
		table_tb = descrs[i].m.tb;
		table_sec = descrs[i].m.sec;
		if ((bp == table_bp || table_bp == X) &&
		    (status->tb == table_tb || table_tb == X) &&
		    (status->sec == table_sec || table_sec == X)) {
			*start = descrs[i].range.start;
			*len = descrs[i].range.len;

			status_found = 1;
			break;
		}
	}

	if (!status_found) {
		msg_cerr("matching status not found\n");
		return -1;
	}

	return 0;
}

/* Given a [start, len], this function calls w25_range_to_status() to convert
 * it to flash-chip-specific range bits, then sets into status register.
 */
static int w25_set_range(const struct flashctx *flash,
                         unsigned int start, unsigned int len)
{
	struct w25q_status status;
	int tmp = 0;
	int expected = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register(flash);
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	if (w25_range_to_status(flash, start, len, &status))
		return -1;

	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.sec: %x\n", status.sec);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	memcpy(&expected, &status, sizeof(status));
	spi_write_status_register(flash, expected);

	tmp = spi_read_status_register(flash);
	msg_cdbg("%s: new status: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP_AREA) != (expected & MASK_WP_AREA)) {
		msg_cerr("expected=0x%02x, but actual=0x%02x.\n",
		          expected, tmp);
		return 1;
	}

	return 0;
}

/* Print out the current status register value with human-readable text. */
static int w25_wp_status(const struct flashctx *flash)
{
	struct w25q_status status;
	int tmp;
	unsigned int start, len;
	int ret = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register(flash);
	memcpy(&status, &tmp, 1);
	msg_cinfo("WP: status: 0x%02x\n", tmp);
	msg_cinfo("WP: status.srp0: %x\n", status.srp0);
	msg_cinfo("WP: write protect is %s.\n",
	          status.srp0 ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &status, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

static int w25q_large_range_to_status(const struct flashctx *flash,
				      unsigned int start, unsigned int len,
				      struct w25q_status_large *status)
{
	struct wp_range_descriptor *descrs;
	int i, range_found = 0;
	int num_entries;

	if (w25_range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		struct wp_range *r = &descrs[i].range;

		msg_cspew("comparing range 0x%x 0x%x / 0x%x 0x%x\n",
			  start, len, r->start, r->len);
		if ((start == r->start) && (len == r->len)) {
			status->bp0 = descrs[i].bp & 1;
			status->bp1 = descrs[i].bp >> 1;
			status->bp2 = descrs[i].bp >> 2;
			status->bp3 = descrs[i].bp >> 3;
			/*
			 * For MX25U12835E chip, Top/Bottom (T/B) bit is not
			 * part of status register and in that bit position is
			 * Quad Enable (QE)
			 */
			if (flash->chip->manufacture_id != MACRONIX_ID ||
			    flash->chip->model_id != MACRONIX_MX25U12835E)
				status->tb = descrs[i].m.tb;

			range_found = 1;
			break;
		}
	}

	if (!range_found) {
		msg_cerr("%s: matching range not found\n", __func__);
		return -1;
	}

	return 0;
}

static int w25_large_status_to_range(const struct flashctx *flash,
				     const struct w25q_status_large *status,
				     unsigned int *start, unsigned int *len)
{
	struct wp_range_descriptor *descrs;
	int i, status_found = 0;
	int num_entries;

	if (w25_range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		int bp;
		int table_bp, table_tb;

		bp = status->bp0 | (status->bp1 << 1) | (status->bp2 << 2) |
			(status->bp3 << 3);
		msg_cspew("comparing  0x%x 0x%x / 0x%x 0x%x\n",
		          bp, descrs[i].bp,
		          status->tb, descrs[i].m.tb);
		table_bp = descrs[i].bp;
		table_tb = descrs[i].m.tb;
		if ((bp == table_bp || table_bp == X) &&
		    (status->tb == table_tb || table_tb == X)) {
			*start = descrs[i].range.start;
			*len = descrs[i].range.len;

			status_found = 1;
			break;
		}
	}

	if (!status_found) {
		msg_cerr("matching status not found\n");
		return -1;
	}

	return 0;
}

/* Given a [start, len], this function calls w25_range_to_status() to convert
 * it to flash-chip-specific range bits, then sets into status register.
 * Returns 0 if successful, -1 on error, and 1 if reading back was different.
 */
static int w25q_large_set_range(const struct flashctx *flash,
				unsigned int start, unsigned int len)
{
	struct w25q_status_large status;
	int tmp;
	int expected = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register(flash);
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	if (w25q_large_range_to_status(flash, start, len, &status))
		return -1;

	msg_cdbg("status.busy: %x\n", status.busy);
	msg_cdbg("status.wel: %x\n", status.wel);
	msg_cdbg("status.bp0: %x\n", status.bp0);
	msg_cdbg("status.bp1: %x\n", status.bp1);
	msg_cdbg("status.bp2: %x\n", status.bp2);
	msg_cdbg("status.bp3: %x\n", status.bp3);
	msg_cdbg("status.tb: %x\n", status.tb);
	msg_cdbg("status.srp0: %x\n", status.srp0);

	memcpy(&expected, &status, sizeof(status));
	spi_write_status_register(flash, expected);

	tmp = spi_read_status_register(flash);
	msg_cdbg("%s: new status: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP_AREA_LARGE) != (expected & MASK_WP_AREA_LARGE)) {
		msg_cerr("expected=0x%02x, but actual=0x%02x.\n",
		          expected, tmp);
		return 1;
	}

	return 0;
}

static int w25q_large_wp_status(const struct flashctx *flash)
{
	struct w25q_status_large sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp[2];
	unsigned int start, len;
	int ret = 0;

	memset(&sr1, 0, sizeof(sr1));
	tmp[0] = spi_read_status_register(flash);
	memcpy(&sr1, &tmp[0], 1);

	memset(&sr2, 0, sizeof(sr2));
	tmp[1] = w25q_read_status_register_2(flash);
	memcpy(&sr2, &tmp[1], 1);

	msg_cinfo("WP: status: 0x%02x%02x\n", tmp[1], tmp[0]);
	msg_cinfo("WP: status.srp0: %x\n", sr1.srp0);
	msg_cinfo("WP: status.srp1: %x\n", sr2.srp1);
	msg_cinfo("WP: write protect is %s.\n",
	          (sr1.srp0 || sr2.srp1) ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_large_status_to_range(flash, &sr1, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

/* Set/clear the SRP0 bit in the status register. */
static int w25_set_srp0(const struct flashctx *flash, int enable)
{
	struct w25q_status status;
	int tmp = 0;
	int expected = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register(flash);
	/* FIXME: this is NOT endian-free copy. */
	memcpy(&status, &tmp, 1);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, tmp);

	status.srp0 = enable ? 1 : 0;
	memcpy(&expected, &status, sizeof(status));
	spi_write_status_register(flash, expected);

	tmp = spi_read_status_register(flash);
	msg_cdbg("%s: new status: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP_AREA) != (expected & MASK_WP_AREA))
		return 1;

	return 0;
}

static int w25_enable_writeprotect(const struct flashctx *flash,
		enum wp_mode wp_mode)
{
	int ret;

	if (wp_mode != WP_MODE_HARDWARE) {
		msg_cerr("%s(): unsupported write-protect mode\n", __func__);
		return 1;
	}

	ret = w25_set_srp0(flash, 1);
	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

static int w25_disable_writeprotect(const struct flashctx *flash)
{
	int ret;

	ret = w25_set_srp0(flash, 0);
	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);

	return ret;
}

static int w25_list_ranges(const struct flashctx *flash)
{
	struct wp_range_descriptor *descrs;
	int i, num_entries;

	if (w25_range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		msg_cinfo("start: 0x%06x, length: 0x%06x\n",
		          descrs[i].range.start,
		          descrs[i].range.len);
	}

	return 0;
}

static int w25q_wp_status(const struct flashctx *flash)
{
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp[2];
	unsigned int start, len;
	int ret = 0;

	memset(&sr1, 0, sizeof(sr1));
	tmp[0] = spi_read_status_register(flash);
	memcpy(&sr1, &tmp[0], 1);

	memset(&sr2, 0, sizeof(sr2));
	tmp[1] = w25q_read_status_register_2(flash);
	memcpy(&sr2, &tmp[1], 1);

	msg_cinfo("WP: status: 0x%02x%02x\n", tmp[1], tmp[0]);
	msg_cinfo("WP: status.srp0: %x\n", sr1.srp0);
	msg_cinfo("WP: status.srp1: %x\n", sr2.srp1);
	msg_cinfo("WP: write protect is %s.\n",
	          (sr1.srp0 || sr2.srp1) ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &sr1, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

/*
 * W25Q adds an optional byte to the standard WRSR opcode. If /CS is
 * de-asserted after the first byte, then it acts like a JEDEC-standard
 * WRSR command. if /CS is asserted, then the next data byte is written
 * into status register 2.
 */
#define W25Q_WRSR_OUTSIZE	0x03
static int w25q_write_status_register_WREN(const struct flashctx *flash, uint8_t s1, uint8_t s2)
{
	int result;
	struct spi_command cmds[] = {
	{
	/* FIXME: WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt       = JEDEC_WREN_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WREN },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = W25Q_WRSR_OUTSIZE,
		.writearr       = (const unsigned char[]){ JEDEC_WRSR, s1, s2 },
		.readcnt        = 0,
		.readarr        = NULL,
	}, {
		.writecnt       = 0,
		.writearr       = NULL,
		.readcnt        = 0,
		.readarr        = NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
	        msg_cerr("%s failed during command execution\n",
	                __func__);
	}

	/* WRSR performs a self-timed erase before the changes take effect. */
	programmer_delay(100 * 1000);

	return result;
}

/*
 * Set/clear the SRP1 bit in status register 2.
 * FIXME: make this more generic if other chips use the same SR2 layout
 */
static int w25q_set_srp1(const struct flashctx *flash, int enable)
{
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp, expected;

	tmp = spi_read_status_register(flash);
	memcpy(&sr1, &tmp, 1);
	tmp = w25q_read_status_register_2(flash);
	memcpy(&sr2, &tmp, 1);

	msg_cdbg("%s: old status 2: 0x%02x\n", __func__, tmp);

	sr2.srp1 = enable ? 1 : 0;

	memcpy(&expected, &sr2, 1);
	w25q_write_status_register_WREN(flash, *((uint8_t *)&sr1), *((uint8_t *)&sr2));

	tmp = w25q_read_status_register_2(flash);
	msg_cdbg("%s: new status 2: 0x%02x\n", __func__, tmp);
	if ((tmp & MASK_WP2_AREA) != (expected & MASK_WP2_AREA))
		return 1;

	return 0;
}

enum wp_mode get_wp_mode(const char *mode_str)
{
	enum wp_mode wp_mode = WP_MODE_UNKNOWN;

	if (!strcasecmp(mode_str, "hardware"))
		wp_mode = WP_MODE_HARDWARE;
	else if (!strcasecmp(mode_str, "power_cycle"))
		wp_mode = WP_MODE_POWER_CYCLE;
	else if (!strcasecmp(mode_str, "permanent"))
		wp_mode = WP_MODE_PERMANENT;

	return wp_mode;
}

static int w25q_disable_writeprotect(const struct flashctx *flash,
		enum wp_mode wp_mode)
{
	int ret = 1;
	struct w25q_status_2 sr2;
	uint8_t tmp;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		ret = w25_set_srp0(flash, 0);
		break;
	case WP_MODE_POWER_CYCLE:
		tmp = w25q_read_status_register_2(flash);
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1) {
			msg_cerr("%s(): must disconnect power to disable "
					"write-protection\n", __func__);
		} else {
			ret = 0;
		}
		break;
	case WP_MODE_PERMANENT:
		msg_cerr("%s(): cannot disable permanent write-protection\n",
				__func__);
		break;
	default:
		msg_cerr("%s(): invalid mode specified\n", __func__);
		break;
	}

	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);

	return ret;
}

static int w25q_disable_writeprotect_default(const struct flashctx *flash)
{
	return w25q_disable_writeprotect(flash, WP_MODE_HARDWARE);
}

static int w25q_enable_writeprotect(const struct flashctx *flash,
		enum wp_mode wp_mode)
{
	int ret = 1;
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp;

	switch (wp_mode) {
	case WP_MODE_HARDWARE:
		if (w25q_disable_writeprotect(flash, WP_MODE_POWER_CYCLE)) {
			msg_cerr("%s(): cannot disable power cycle WP mode\n",
					__func__);
			break;
		}

		tmp = spi_read_status_register(flash);
		memcpy(&sr1, &tmp, 1);
		if (sr1.srp0)
			ret = 0;
		else
			ret = w25_set_srp0(flash, 1);

		break;
	case WP_MODE_POWER_CYCLE:
		if (w25q_disable_writeprotect(flash, WP_MODE_HARDWARE)) {
			msg_cerr("%s(): cannot disable hardware WP mode\n",
					__func__);
			break;
		}

		tmp = w25q_read_status_register_2(flash);
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1)
			ret = 0;
		else
			ret = w25q_set_srp1(flash, 1);

		break;
	case WP_MODE_PERMANENT:
		tmp = spi_read_status_register(flash);
		memcpy(&sr1, &tmp, 1);
		if (sr1.srp0 == 0) {
			ret = w25_set_srp0(flash, 1);
			if (ret) {
				msg_perr("%s(): cannot enable SRP0 for "
						"permanent WP\n", __func__);
				break;
			}
		}

		tmp = w25q_read_status_register_2(flash);
		memcpy(&sr2, &tmp, 1);
		if (sr2.srp1 == 0) {
			ret = w25q_set_srp1(flash, 1);
			if (ret) {
				msg_perr("%s(): cannot enable SRP1 for "
						"permanent WP\n", __func__);
				break;
			}
		}

		break;
	default:
		msg_perr("%s(): invalid mode %d\n", __func__, wp_mode);
		break;
	}

	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);
	return ret;
}

/* W25P, W25X, and many flash chips from various vendors */
struct wp wp_w25 = {
	.list_ranges	= w25_list_ranges,
	.set_range	= w25_set_range,
	.enable		= w25_enable_writeprotect,
	.disable	= w25_disable_writeprotect,
	.wp_status	= w25_wp_status,

};

/* W25Q series has features such as a second status register and SFDP */
struct wp wp_w25q = {
	.list_ranges	= w25_list_ranges,
	.set_range	= w25_set_range,
	.enable		= w25q_enable_writeprotect,
	/*
	 * By default, disable hardware write-protection. We may change
	 * this later if we want to add fine-grained write-protect disable
	 * as a command-line option.
	 */
	.disable	= w25q_disable_writeprotect_default,
	.wp_status	= w25q_wp_status,
};

/* W25Q large series has 4 block-protect bits */
struct wp wp_w25q_large = {
	.list_ranges	= w25_list_ranges,
	.set_range	= w25q_large_set_range,
	.enable		= w25q_enable_writeprotect,
	/*
	 * By default, disable hardware write-protection. We may change
	 * this later if we want to add fine-grained write-protect disable
	 * as a command-line option.
	 */
	.disable	= w25q_disable_writeprotect_default,
	.wp_status	= w25q_large_wp_status,
};

static struct wp_range_descriptor gd25q32_cmp0_ranges[] = {
	/* none, bp4 and bp3 => don't care */
	{ { }, 0x00, {0, 0} },
	{ { }, 0x08, {0, 0} },
	{ { }, 0x10, {0, 0} },
	{ { }, 0x18, {0, 0} },

	{ { }, 0x01, {0x3f0000, 64 * 1024} },
	{ { }, 0x02, {0x3e0000, 128 * 1024} },
	{ { }, 0x03, {0x3c0000, 256 * 1024} },
	{ { }, 0x04, {0x380000, 512 * 1024} },
	{ { }, 0x05, {0x300000, 1024 * 1024} },
	{ { }, 0x06, {0x200000, 2048 * 1024} },

	{ { }, 0x09, {0x000000, 64 * 1024} },
	{ { }, 0x0a, {0x000000, 128 * 1024} },
	{ { }, 0x0b, {0x000000, 256 * 1024} },
	{ { }, 0x0c, {0x000000, 512 * 1024} },
	{ { }, 0x0d, {0x000000, 1024 * 1024} },
	{ { }, 0x0e, {0x000000, 2048 * 1024} },

	/* all, bp4 and bp3 => don't care */
	{ { }, 0x07, {0x000000, 4096 * 1024} },
	{ { }, 0x0f, {0x000000, 4096 * 1024} },
	{ { }, 0x17, {0x000000, 4096 * 1024} },
	{ { }, 0x1f, {0x000000, 4096 * 1024} },

	{ { }, 0x11, {0x3ff000, 4 * 1024} },
	{ { }, 0x12, {0x3fe000, 8 * 1024} },
	{ { }, 0x13, {0x3fc000, 16 * 1024} },
	{ { }, 0x14, {0x3f8000, 32 * 1024} },	/* bp0 => don't care */
	{ { }, 0x15, {0x3f8000, 32 * 1024} },	/* bp0 => don't care */
	{ { }, 0x16, {0x3f8000, 32 * 1024} },

	{ { }, 0x19, {0x000000, 4 * 1024} },
	{ { }, 0x1a, {0x000000, 8 * 1024} },
	{ { }, 0x1b, {0x000000, 16 * 1024} },
	{ { }, 0x1c, {0x000000, 32 * 1024} },	/* bp0 => don't care */
	{ { }, 0x1d, {0x000000, 32 * 1024} },	/* bp0 => don't care */
	{ { }, 0x1e, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor gd25q32_cmp1_ranges[] = {
	/* All, bp4 and bp3 => don't care */
	{ { }, 0x00, {0x000000, 4096 * 1024} }, /* All */
	{ { }, 0x08, {0x000000, 4096 * 1024} },
	{ { }, 0x10, {0x000000, 4096 * 1024} },
	{ { }, 0x18, {0x000000, 4096 * 1024} },

	{ { }, 0x01, {0x000000, 4032 * 1024} },
	{ { }, 0x02, {0x000000, 3968 * 1024} },
	{ { }, 0x03, {0x000000, 3840 * 1024} },
	{ { }, 0x04, {0x000000, 3584 * 1024} },
	{ { }, 0x05, {0x000000, 3 * 1024 * 1024} },
	{ { }, 0x06, {0x000000, 2 * 1024 * 1024} },

	{ { }, 0x09, {0x010000, 4032 * 1024} },
	{ { }, 0x0a, {0x020000, 3968 * 1024} },
	{ { }, 0x0b, {0x040000, 3840 * 1024} },
	{ { }, 0x0c, {0x080000, 3584 * 1024} },
	{ { }, 0x0d, {0x100000, 3 * 1024 * 1024} },
	{ { }, 0x0e, {0x200000, 2 * 1024 * 1024} },

	/* None, bp4 and bp3 => don't care */
	{ { }, 0x07, {0, 0} }, /* None */
	{ { }, 0x0f, {0, 0} },
	{ { }, 0x17, {0, 0} },
	{ { }, 0x1f, {0, 0} },

	{ { }, 0x11, {0x000000, 4092 * 1024} },
	{ { }, 0x12, {0x000000, 4088 * 1024} },
	{ { }, 0x13, {0x000000, 4080 * 1024} },
	{ { }, 0x14, {0x000000, 4064 * 1024} },	/* bp0 => don't care */
	{ { }, 0x15, {0x000000, 4064 * 1024} },	/* bp0 => don't care */
	{ { }, 0x16, {0x000000, 4064 * 1024} },

	{ { }, 0x19, {0x001000, 4092 * 1024} },
	{ { }, 0x1a, {0x002000, 4088 * 1024} },
	{ { }, 0x1b, {0x040000, 4080 * 1024} },
	{ { }, 0x1c, {0x080000, 4064 * 1024} },	/* bp0 => don't care */
	{ { }, 0x1d, {0x080000, 4064 * 1024} },	/* bp0 => don't care */
	{ { }, 0x1e, {0x080000, 4064 * 1024} },
};

static struct status_register_layout gd25q32_sr1 = {
	/* TODO: map second status register */
	.bp0_pos = 2, .bp_bits = 5, .srp_pos = 7
};

static struct wp_range_descriptor gd25q128_cmp0_ranges[] = {
	/* none, bp4 and bp3 => don't care, others = 0 */
	{ { .tb = 0  }, 0x00, {0, 0} },
	{ { .tb = 0  }, 0x08, {0, 0} },
	{ { .tb = 0  }, 0x10, {0, 0} },
	{ { .tb = 0  }, 0x18, {0, 0} },

	{ { .tb = 0 }, 0x01, {0xfc0000, 256 * 1024} },
	{ { .tb = 0 }, 0x02, {0xf80000, 512 * 1024} },
	{ { .tb = 0 }, 0x03, {0xf00000, 1024 * 1024} },
	{ { .tb = 0 }, 0x04, {0xe00000, 2048 * 1024} },
	{ { .tb = 0 }, 0x05, {0xc00000, 4096 * 1024} },
	{ { .tb = 0 }, 0x06, {0x800000, 8192 * 1024} },

	{ { .tb = 0 }, 0x09, {0x000000, 256 * 1024} },
	{ { .tb = 0 }, 0x0a, {0x000000, 512 * 1024} },
	{ { .tb = 0 }, 0x0b, {0x000000, 1024 * 1024} },
	{ { .tb = 0 }, 0x0c, {0x000000, 2048 * 1024} },
	{ { .tb = 0 }, 0x0d, {0x000000, 4096 * 1024} },
	{ { .tb = 0 }, 0x0e, {0x000000, 8192 * 1024} },

	/* all, bp4 and bp3 => don't care, others = 1 */
	{ { .tb = 0 }, 0x07, {0x000000, 16384 * 1024} },
	{ { .tb = 0 }, 0x0f, {0x000000, 16384 * 1024} },
	{ { .tb = 0 }, 0x17, {0x000000, 16384 * 1024} },
	{ { .tb = 0 }, 0x1f, {0x000000, 16384 * 1024} },

	{ { .tb = 0 }, 0x11, {0xfff000, 4 * 1024} },
	{ { .tb = 0 }, 0x12, {0xffe000, 8 * 1024} },
	{ { .tb = 0 }, 0x13, {0xffc000, 16 * 1024} },
	{ { .tb = 0 }, 0x14, {0xff8000, 32 * 1024} },	/* bp0 => don't care */
	{ { .tb = 0 }, 0x15, {0xff8000, 32 * 1024} },	/* bp0 => don't care */

	{ { .tb = 0 }, 0x19, {0x000000, 4 * 1024} },
	{ { .tb = 0 }, 0x1a, {0x000000, 8 * 1024} },
	{ { .tb = 0 }, 0x1b, {0x000000, 16 * 1024} },
	{ { .tb = 0 }, 0x1c, {0x000000, 32 * 1024} },	/* bp0 => don't care */
	{ { .tb = 0 }, 0x1d, {0x000000, 32 * 1024} },	/* bp0 => don't care */
	{ { .tb = 0 }, 0x1e, {0x000000, 32 * 1024} },
};

static struct wp_range_descriptor gd25q128_cmp1_ranges[] = {
	/* none, bp4 and bp3 => don't care, others = 0 */
	{ { .tb = 1 }, 0x00, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x08, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x10, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x18, {0x000000, 16384 * 1024} },

	{ { .tb = 1 }, 0x01, {0x000000, 16128 * 1024} },
	{ { .tb = 1 }, 0x02, {0x000000, 15872 * 1024} },
	{ { .tb = 1 }, 0x03, {0x000000, 15360 * 1024} },
	{ { .tb = 1 }, 0x04, {0x000000, 14336 * 1024} },
	{ { .tb = 1 }, 0x05, {0x000000, 12288 * 1024} },
	{ { .tb = 1 }, 0x06, {0x000000, 8192 * 1024} },

	{ { .tb = 1 }, 0x09, {0x000000, 16128 * 1024} },
	{ { .tb = 1 }, 0x0a, {0x000000, 15872 * 1024} },
	{ { .tb = 1 }, 0x0b, {0x000000, 15360 * 1024} },
	{ { .tb = 1 }, 0x0c, {0x000000, 14336 * 1024} },
	{ { .tb = 1 }, 0x0d, {0x000000, 12288 * 1024} },
	{ { .tb = 1 }, 0x0e, {0x000000, 8192 * 1024} },

	/* none, bp4 and bp3 => don't care, others = 1 */
	{ { .tb = 1 }, 0x07, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x08, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x0f, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x17, {0x000000, 16384 * 1024} },
	{ { .tb = 1 }, 0x1f, {0x000000, 16384 * 1024} },

	{ { .tb = 1 }, 0x11, {0x000000, 16380 * 1024} },
	{ { .tb = 1 }, 0x12, {0x000000, 16376 * 1024} },
	{ { .tb = 1 }, 0x13, {0x000000, 16368 * 1024} },
	{ { .tb = 1 }, 0x14, {0x000000, 16352 * 1024} },	/* bp0 => don't care */
	{ { .tb = 1 }, 0x15, {0x000000, 16352 * 1024} },	/* bp0 => don't care */

	{ { .tb = 1 }, 0x19, {0x001000, 16380 * 1024} },
	{ { .tb = 1 }, 0x1a, {0x002000, 16376 * 1024} },
	{ { .tb = 1 }, 0x1b, {0x004000, 16368 * 1024} },
	{ { .tb = 1 }, 0x1c, {0x008000, 16352 * 1024} },	/* bp0 => don't care */
	{ { .tb = 1 }, 0x1d, {0x008000, 16352 * 1024} },	/* bp0 => don't care */
	{ { .tb = 1 }, 0x1e, {0x008000, 16352 * 1024} },
};

static struct status_register_layout gd25q128_sr1 = {
	/* TODO: map second and third status registers */
	.bp0_pos = 2, .bp_bits = 5, .srp_pos = 7
};

/* FIXME: MX25L6406 has same ID as MX25L6405D */
static struct wp_range_descriptor mx25l6406e_ranges[] = {
	{ { }, 0, {0, 0} },	/* none */
	{ { }, 0x1, {0x7e0000, 64 * 2 * 1024} },	/* blocks 126-127 */
	{ { }, 0x2, {0x7c0000, 64 * 4 * 1024} },	/* blocks 124-127 */
	{ { }, 0x3, {0x7a0000, 64 * 8 * 1024} },	/* blocks 120-127 */
	{ { }, 0x4, {0x700000, 64 * 16 * 1024} },	/* blocks 112-127 */
	{ { }, 0x5, {0x600000, 64 * 32 * 1024} },	/* blocks 96-127 */
	{ { }, 0x6, {0x400000, 64 * 64 * 1024} },	/* blocks 64-127 */

	{ { }, 0x7, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0x8, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0x9, {0x000000, 64 * 64 * 1024} },	/* blocks 0-63 */
	{ { }, 0xa, {0x000000, 64 * 96 * 1024} },	/* blocks 0-95 */
	{ { }, 0xb, {0x000000, 64 * 112 * 1024} },	/* blocks 0-111 */
	{ { }, 0xc, {0x000000, 64 * 120 * 1024} },	/* blocks 0-119 */
	{ { }, 0xd, {0x000000, 64 * 124 * 1024} },	/* blocks 0-123 */
	{ { }, 0xe, {0x000000, 64 * 126 * 1024} },	/* blocks 0-125 */
	{ { }, 0xf, {0x000000, 64 * 128 * 1024} },	/* all */
};

static struct status_register_layout mx25l6406e_sr1 = {
	.bp0_pos = 2, .bp_bits = 4, .srp_pos = 7
};

static struct wp_range_descriptor mx25l6495f_tb0_ranges[] = {
	{ { }, 0, {0, 0} },	/* none */
	{ { }, 0x1, {0x7f0000, 64 * 1 * 1024} },	/* block 127 */
	{ { }, 0x2, {0x7e0000, 64 * 2 * 1024} },	/* blocks 126-127 */
	{ { }, 0x3, {0x7c0000, 64 * 4 * 1024} },	/* blocks 124-127 */

	{ { }, 0x4, {0x780000, 64 * 8 * 1024} },	/* blocks 120-127 */
	{ { }, 0x5, {0x700000, 64 * 16 * 1024} },	/* blocks 112-127 */
	{ { }, 0x6, {0x600000, 64 * 32 * 1024} },	/* blocks 96-127 */
	{ { }, 0x7, {0x400000, 64 * 64 * 1024} },	/* blocks 64-127 */
	{ { }, 0x8, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0x9, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xa, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xb, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xc, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xd, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xe, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xf, {0x000000, 64 * 128 * 1024} },	/* all */
};

static struct wp_range_descriptor mx25l6495f_tb1_ranges[] = {
	{ { }, 0, {0, 0} },	/* none */
	{ { }, 0x1, {0x000000, 64 * 1 * 1024} },	/* block 0 */
	{ { }, 0x2, {0x000000, 64 * 2 * 1024} },	/* blocks 0-1 */
	{ { }, 0x3, {0x000000, 64 * 4 * 1024} },	/* blocks 0-3 */
	{ { }, 0x4, {0x000000, 64 * 8 * 1024} },	/* blocks 0-7 */
	{ { }, 0x5, {0x000000, 64 * 16 * 1024} },	/* blocks 0-15 */
	{ { }, 0x6, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
	{ { }, 0x7, {0x000000, 64 * 64 * 1024} },	/* blocks 0-63 */
	{ { }, 0x8, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0x9, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xa, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xb, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xc, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xd, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xe, {0x000000, 64 * 128 * 1024} },	/* all */
	{ { }, 0xf, {0x000000, 64 * 128 * 1024} },	/* all */
};

static struct status_register_layout mx25l6495f_sr1 = {
	.bp0_pos = 2, .bp_bits = 4, .srp_pos = 7
};

static struct wp_range_descriptor mx25l25635f_tb0_ranges[] = {
	{ { }, 0, {0, 0} },	/* none */
	{ { }, 0x1, {0x1ff0000, 64 * 1 * 1024} },	/* block 511 */
	{ { }, 0x2, {0x1fe0000, 64 * 2 * 1024} },	/* blocks 510-511 */
	{ { }, 0x3, {0x1fc0000, 64 * 4 * 1024} },	/* blocks 508-511 */
	{ { }, 0x4, {0x1f80000, 64 * 8 * 1024} },	/* blocks 504-511 */
	{ { }, 0x5, {0x1f00000, 64 * 16 * 1024} },	/* blocks 496-511 */
	{ { }, 0x6, {0x1e00000, 64 * 32 * 1024} },	/* blocks 480-511 */
	{ { }, 0x7, {0x1c00000, 64 * 64 * 1024} },	/* blocks 448-511 */
	{ { }, 0x8, {0x1800000, 64 * 128 * 1024} },	/* blocks 384-511 */
	{ { }, 0x9, {0x1000000, 64 * 256 * 1024} },	/* blocks 256-511 */
	{ { }, 0xa, {0x0000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xb, {0x0000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xc, {0x0000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xd, {0x0000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xe, {0x0000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xf, {0x0000000, 64 * 512 * 1024} },	/* all */
};

static struct wp_range_descriptor mx25l25635f_tb1_ranges[] = {
	{ { }, 0, {0, 0} },	/* none */
	{ { }, 0x1, {0x000000, 64 * 1 * 1024} },	/* block 0 */
	{ { }, 0x2, {0x000000, 64 * 2 * 1024} },	/* blocks 0-1 */
	{ { }, 0x3, {0x000000, 64 * 4 * 1024} },	/* blocks 0-3 */
	{ { }, 0x4, {0x000000, 64 * 8 * 1024} },	/* blocks 0-7 */
	{ { }, 0x5, {0x000000, 64 * 16 * 1024} },	/* blocks 0-15 */
	{ { }, 0x6, {0x000000, 64 * 32 * 1024} },	/* blocks 0-31 */
	{ { }, 0x7, {0x000000, 64 * 64 * 1024} },	/* blocks 0-63 */
	{ { }, 0x8, {0x000000, 64 * 128 * 1024} },	/* blocks 0-127 */
	{ { }, 0x9, {0x000000, 64 * 256 * 1024} },	/* blocks 0-255 */
	{ { }, 0xa, {0x000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xb, {0x000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xc, {0x000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xd, {0x000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xe, {0x000000, 64 * 512 * 1024} },	/* all */
	{ { }, 0xf, {0x000000, 64 * 512 * 1024} },	/* all */
};

static struct status_register_layout mx25l25635f_sr1 = {
	.bp0_pos = 2, .bp_bits = 4, .srp_pos = 7
};

static struct wp_range_descriptor s25fs128s_ranges[] = {
	{ { .tb = 1 }, 0, {0, 0} },	/* none */
	{ { .tb = 1 }, 0x1, {0x000000, 256 * 1024} },	/* lower 64th */
	{ { .tb = 1 }, 0x2, {0x000000, 512 * 1024} },	/* lower 32nd */
	{ { .tb = 1 }, 0x3, {0x000000, 1024 * 1024} },	/* lower 16th */
	{ { .tb = 1 }, 0x4, {0x000000, 2048 * 1024} },	/* lower 8th */
	{ { .tb = 1 }, 0x5, {0x000000, 4096 * 1024} },	/* lower 4th */
	{ { .tb = 1 }, 0x6, {0x000000, 8192 * 1024} },	/* lower half */
	{ { .tb = 1 }, 0x7, {0x000000, 16384 * 1024} },	/* all */

	{ { .tb = 0 }, 0, {0, 0} },	/* none */
	{ { .tb = 0 }, 0x1, {0xfc0000, 256 * 1024} },	/* upper 64th */
	{ { .tb = 0 }, 0x2, {0xf80000, 512 * 1024} },	/* upper 32nd */
	{ { .tb = 0 }, 0x3, {0xf00000, 1024 * 1024} },	/* upper 16th */
	{ { .tb = 0 }, 0x4, {0xe00000, 2048 * 1024} },	/* upper 8th */
	{ { .tb = 0 }, 0x5, {0xc00000, 4096 * 1024} },	/* upper 4th */
	{ { .tb = 0 }, 0x6, {0x800000, 8192 * 1024} },	/* upper half */
	{ { .tb = 0 }, 0x7, {0x000000, 16384 * 1024} },	/* all */
};

static struct status_register_layout s25fs128s_sr1 = {
	.bp0_pos = 2, .bp_bits = 3, .srp_pos = 7
};


static struct wp_range_descriptor s25fl256s_ranges[] = {
	{ { .tb = 1 }, 0, {0, 0} },	/* none */
	{ { .tb = 1 }, 0x1, {0x000000, 512 * 1024} },		/* lower 64th */
	{ { .tb = 1 }, 0x2, {0x000000, 1024 * 1024} },		/* lower 32nd */
	{ { .tb = 1 }, 0x3, {0x000000, 2048 * 1024} },		/* lower 16th */
	{ { .tb = 1 }, 0x4, {0x000000, 4096 * 1024} },		/* lower 8th */
	{ { .tb = 1 }, 0x5, {0x000000, 8192 * 1024} },		/* lower 4th */
	{ { .tb = 1 }, 0x6, {0x000000, 16384 * 1024} },		/* lower half */
	{ { .tb = 1 }, 0x7, {0x000000, 32768 * 1024} },		/* all */

	{ { .tb = 0 }, 0, {0, 0} },	/* none */
	{ { .tb = 0 }, 0x1, {0x1f80000, 512 * 1024} },		/* upper 64th */
	{ { .tb = 0 }, 0x2, {0x1f00000, 1024 * 1024} },		/* upper 32nd */
	{ { .tb = 0 }, 0x3, {0x1e00000, 2048 * 1024} },		/* upper 16th */
	{ { .tb = 0 }, 0x4, {0x1c00000, 4096 * 1024} },		/* upper 8th */
	{ { .tb = 0 }, 0x5, {0x1800000, 8192 * 1024} },		/* upper 4th */
	{ { .tb = 0 }, 0x6, {0x1000000, 16384 * 1024} },	/* upper half */
	{ { .tb = 0 }, 0x7, {0x000000, 32768 * 1024} },		/* all */
};

static struct status_register_layout s25fl256s_sr1 = {
	.bp0_pos = 2, .bp_bits = 3, .srp_pos = 7
};

static int get_sr1_layout(
	const struct flashctx *flash, struct status_register_layout *sr1)
{
	switch (flash->chip->manufacture_id) {
	case GIGADEVICE_ID:
		switch(flash->chip->model_id) {

		case GIGADEVICE_GD25Q32:
			*sr1 = gd25q32_sr1;
			return 0;
		case GIGADEVICE_GD25LQ128CD:
			*sr1 = gd25q128_sr1;
			return 0;
		}
		break;
	case MACRONIX_ID:
		switch (flash->chip->model_id) {
		case MACRONIX_MX25L6405:
			*sr1 = mx25l6406e_sr1;
			return 0;
		case MACRONIX_MX25L6495F:
			*sr1 = mx25l6495f_sr1;
			return 0;
		case MACRONIX_MX25L25635F:
			*sr1 = mx25l25635f_sr1;
			return 0;
		}
		break;
	case SPANSION_ID:
		switch (flash->chip->model_id) {
		case SPANSION_S25FS128S_L:
		case SPANSION_S25FS128S_S:
			*sr1 = s25fs128s_sr1;
			return 0;
		case SPANSION_S25FL256S_UL:
		case SPANSION_S25FL256S_US:
			*sr1 = s25fl256s_sr1;
			return 0;
		}
		break;
	}

	return 1;
}

/* Given a flash chip, this function returns its writeprotect info. */
static int generic_range_table(const struct flashctx *flash,
                           struct wp_range_descriptor **descrs,
                           int *num_entries)
{
	*num_entries = 0;

	switch (flash->chip->manufacture_id) {
	case WINBOND_NEX_ID:
		switch(flash->chip->model_id) {
		case WINBOND_NEX_W25X10:
			*descrs = w25x10_ranges;
			*num_entries = ARRAY_SIZE(w25x10_ranges);
			break;
		case WINBOND_NEX_W25X20:
			*descrs = w25x20_ranges;
			*num_entries = ARRAY_SIZE(w25x20_ranges);
			break;
		case WINBOND_NEX_W25X40:
			*descrs = w25x40_ranges;
			*num_entries = ARRAY_SIZE(w25x40_ranges);
			break;
		case WINBOND_NEX_W25X80:
			*descrs = w25x80_ranges;
			*num_entries = ARRAY_SIZE(w25x80_ranges);
			break;
		case WINBOND_NEX_W25Q80_V:
			*descrs = w25q80_ranges;
			*num_entries = ARRAY_SIZE(w25q80_ranges);
			break;
		case WINBOND_NEX_W25Q16_V:
			*descrs = w25q16_ranges;
			*num_entries = ARRAY_SIZE(w25q16_ranges);
			break;
		case WINBOND_NEX_W25Q32_V:
		case WINBOND_NEX_W25Q32_W:
		case WINBOND_NEX_W25Q32JW:
			*descrs = w25q32_ranges;
			*num_entries = ARRAY_SIZE(w25q32_ranges);
			break;
		case WINBOND_NEX_W25Q64_V:
                case WINBOND_NEX_W25Q64_W:
			*descrs = w25q64_ranges;
			*num_entries = ARRAY_SIZE(w25q64_ranges);
			break;
		case WINBOND_NEX_W25Q128_DTR:
		case WINBOND_NEX_W25Q128_V_M:
		case WINBOND_NEX_W25Q128_V:
		case WINBOND_NEX_W25Q128_W:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = w25rq128_cmp1_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp1_ranges);
			} else {
				/* CMP == 0 */
				*descrs = w25rq128_cmp0_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp0_ranges);
			}
			break;
		case WINBOND_NEX_W25Q256_V:
		case WINBOND_NEX_W25Q256JV_M:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = w25rq256_cmp1_ranges;
				*num_entries = ARRAY_SIZE(w25rq256_cmp1_ranges);
			} else {
				/* CMP == 0 */
				*descrs = w25rq256_cmp0_ranges;
				*num_entries = ARRAY_SIZE(w25rq256_cmp0_ranges);
			}
			break;
		default:
			msg_cerr("%s() %d: WINBOND flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->chip->model_id);
			return -1;
		}
		break;

	case EON_ID_NOPREFIX:
		switch (flash->chip->model_id) {
		case EON_EN25F40:
			*descrs = en25f40_ranges;
			*num_entries = ARRAY_SIZE(en25f40_ranges);
			break;
		case EON_EN25Q40:
			*descrs = en25q40_ranges;
			*num_entries = ARRAY_SIZE(en25q40_ranges);
			break;
		case EON_EN25Q80:
			*descrs = en25q80_ranges;
			*num_entries = ARRAY_SIZE(en25q80_ranges);
			break;
		case EON_EN25Q32:
			*descrs = en25q32_ranges;
			*num_entries = ARRAY_SIZE(en25q32_ranges);
			break;
		case EON_EN25Q64:
			*descrs = en25q64_ranges;
			*num_entries = ARRAY_SIZE(en25q64_ranges);
			break;
		case EON_EN25Q128:
			*descrs = en25q128_ranges;
			*num_entries = ARRAY_SIZE(en25q128_ranges);
			break;
		case EON_EN25QH128:
			if (w25q_read_status_register_2(flash) & (1 << 6)) {
				/* CMP == 1 */
				*descrs = w25rq128_cmp1_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp1_ranges);
			} else {
				/* CMP == 0 */
				*descrs = w25rq128_cmp0_ranges;
				*num_entries = ARRAY_SIZE(w25rq128_cmp0_ranges);
			}
			break;
		case EON_EN25S64:
			*descrs = en25s64_ranges;
			*num_entries = ARRAY_SIZE(en25s64_ranges);
			break;
		default:
			msg_cerr("%s():%d: EON flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;

	case GIGADEVICE_ID:
		switch(flash->chip->model_id) {

		case GIGADEVICE_GD25Q32: {
			uint8_t sr1 = w25q_read_status_register_2(flash);

			if (!(sr1 & (1 << 6))) {	/* CMP == 0 */
				*descrs = &gd25q32_cmp0_ranges[0];
				*num_entries = ARRAY_SIZE(gd25q32_cmp0_ranges);
			} else {			/* CMP == 1 */
				*descrs = &gd25q32_cmp1_ranges[0];
				*num_entries = ARRAY_SIZE(gd25q32_cmp1_ranges);
			}

			break;
		}
		case GIGADEVICE_GD25LQ128CD: {
			uint8_t sr1 = w25q_read_status_register_2(flash);

			if (!(sr1 & (1 << 6))) {	/* CMP == 0 */
				*descrs = &gd25q128_cmp0_ranges[0];
				*num_entries = ARRAY_SIZE(gd25q128_cmp0_ranges);
			} else {			/* CMP == 1 */
				*descrs = &gd25q128_cmp1_ranges[0];
				*num_entries = ARRAY_SIZE(gd25q128_cmp1_ranges);
			}

			break;
		}
		default:
			msg_cerr("%s() %d: GigaDevice flash chip mismatch"
				 " (0x%04x), aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;
	case MACRONIX_ID:
		switch (flash->chip->model_id) {
		case MACRONIX_MX25L6405:
			/* FIXME: MX25L64* chips have mixed capabilities and
			   share IDs */
			*descrs = &mx25l6406e_ranges[0];
			*num_entries = ARRAY_SIZE(mx25l6406e_ranges);
			break;
		case MACRONIX_MX25L6495F: {
			uint8_t cr = mx25l_read_config_register(flash);

			if (!(cr & (1 << 3))) {	/* T/B == 0 */
				*descrs = &mx25l6495f_tb0_ranges[0];
				*num_entries = ARRAY_SIZE(mx25l6495f_tb0_ranges);
			} else {		/* T/B == 1 */
				*descrs = &mx25l6495f_tb1_ranges[0];
				*num_entries = ARRAY_SIZE(mx25l6495f_tb1_ranges);
			}
			break;
		 }
		case MACRONIX_MX25L25635F: {
			uint8_t cr = mx25l_read_config_register(flash);

			if (!(cr & (1 << 3))) {	/* T/B == 0 */
				*descrs = &mx25l25635f_tb0_ranges[0];
				*num_entries = ARRAY_SIZE(mx25l25635f_tb0_ranges);
			} else {		/* T/B == 1 */
				*descrs = &mx25l25635f_tb1_ranges[0];
				*num_entries = ARRAY_SIZE(mx25l25635f_tb1_ranges);
			}
			break;
		}
		case MACRONIX_MX25L1005:
			*descrs = mx25l1005_ranges;
			*num_entries = ARRAY_SIZE(mx25l1005_ranges);
			break;
		case MACRONIX_MX25L2005:
			*descrs = mx25l2005_ranges;
			*num_entries = ARRAY_SIZE(mx25l2005_ranges);
			break;
		case MACRONIX_MX25L4005:
			*descrs = mx25l4005_ranges;
			*num_entries = ARRAY_SIZE(mx25l4005_ranges);
			break;
		case MACRONIX_MX25L8005:
			*descrs = mx25l8005_ranges;
			*num_entries = ARRAY_SIZE(mx25l8005_ranges);
			break;
		case MACRONIX_MX25L1605:
			/* FIXME: MX25L1605 and MX25L1605D have different write
			 * protection capabilities, but share IDs */
			*descrs = mx25l1605d_ranges;
			*num_entries = ARRAY_SIZE(mx25l1605d_ranges);
			break;
		case MACRONIX_MX25L3205:
			*descrs = mx25l3205d_ranges;
			*num_entries = ARRAY_SIZE(mx25l3205d_ranges);
			break;
		case MACRONIX_MX25U3235E:
			*descrs = mx25u3235e_ranges;
			*num_entries = ARRAY_SIZE(mx25u3235e_ranges);
			break;
		case MACRONIX_MX25U6435E:
			*descrs = mx25u6435e_ranges;
			*num_entries = ARRAY_SIZE(mx25u6435e_ranges);
			break;
		case MACRONIX_MX25U12835E: {
			uint8_t cr = mx25l_read_config_register(flash);
			if (cr & MX25U12835E_TB) {	/* T/B == 1 */
				*descrs = mx25u12835e_tb1_ranges;
				*num_entries = ARRAY_SIZE(mx25u12835e_tb1_ranges);
			} else {			/* T/B == 0 */
				*descrs = mx25u12835e_tb0_ranges;
				*num_entries = ARRAY_SIZE(mx25u12835e_tb0_ranges);
			}
		}
			break;
		default:
			msg_cerr("%s():%d: MXIC flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->chip->model_id);
			return -1;
		}
		break;
	case SPANSION_ID:
		switch (flash->chip->model_id) {
		case SPANSION_S25FS128S_L:
		case SPANSION_S25FS128S_S: {
			*descrs = s25fs128s_ranges;
			*num_entries = ARRAY_SIZE(s25fs128s_ranges);
			break;
		}
		case SPANSION_S25FL256S_UL:
		case SPANSION_S25FL256S_US: {
			*descrs = s25fl256s_ranges;
			*num_entries = ARRAY_SIZE(s25fl256s_ranges);
			break;
		}
		default:
			msg_cerr("%s():%d Spansion flash chip mismatch (0x%04x)"
				", aborting\n", __func__, __LINE__,
				flash->chip->model_id);
			return -1;
		}
		break;
	default:
		msg_cerr("%s: flash vendor (0x%x) not found, aborting\n",
		         __func__, flash->chip->manufacture_id);
		return -1;
	}

	return 0;
}

/* Determines if special s25f-specific functions need to be used to access a
 * given chip's modifier bits. Very much a hard-coded special case hack, but it
 * is also very easy to replace once a proper abstraction for accessing
 * specific modifier bits is added.  */
static int use_s25f_modifier_bits(const struct flashctx *flash)
{
	bool model_match =
		flash->chip->model_id == SPANSION_S25FS128S_L ||
		flash->chip->model_id == SPANSION_S25FS128S_S ||
		flash->chip->model_id == SPANSION_S25FL256S_UL ||
		flash->chip->model_id == SPANSION_S25FL256S_US;
	return (flash->chip->manufacture_id == SPANSION_ID) && model_match;
}

static uint8_t generic_get_bp_mask(struct status_register_layout sr1)
{
	return ((1 << (sr1.bp0_pos + sr1.bp_bits)) - 1) ^ \
		  ((1 << sr1.bp0_pos) - 1);
}

static uint8_t generic_get_status_check_mask(struct status_register_layout sr1)
{
	return generic_get_bp_mask(sr1) | 1 << sr1.srp_pos;
}

/* Given a [start, len], this function finds a block protect bit combination
 * (if possible) and sets the corresponding bits in "status". Remaining bits
 * are preserved. */
static int generic_range_to_status(const struct flashctx *flash,
                        unsigned int start, unsigned int len,
                        uint8_t *status, uint8_t *check_mask)
{
	struct status_register_layout sr1;
	struct wp_range_descriptor *r;
	int i, range_found = 0, num_entries;
	uint8_t bp_mask;

	if (get_sr1_layout(flash, &sr1))
		return -1;

	if (generic_range_table(flash, &r, &num_entries))
		return -1;

	bp_mask = generic_get_bp_mask(sr1);

	for (i = 0; i < num_entries; i++, r++) {
		msg_cspew("comparing range 0x%x 0x%x / 0x%x 0x%x\n",
			  start, len, r->range.start, r->range.len);
		if ((start == r->range.start) && (len == r->range.len)) {
			*status &= ~(bp_mask);
			*status |= r->bp << (sr1.bp0_pos);

			if (use_s25f_modifier_bits(flash)) {
				if (s25f_set_modifier_bits(flash, &r->m) < 0) {
					msg_cerr("error setting modifier bits for range.\n");
					return -1;
				}
			}

			range_found = 1;
			break;
		}
	}

	if (!range_found) {
		msg_cerr("%s: matching range not found\n", __func__);
		return -1;
	}

	*check_mask = generic_get_status_check_mask(sr1);
	return 0;
}

static int generic_status_to_range(const struct flashctx *flash,
		const uint8_t sr1, unsigned int *start, unsigned int *len)
{
	struct status_register_layout sr1_layout;
	struct wp_range_descriptor *r;
	int num_entries, i, status_found = 0;
	uint8_t sr1_bp;
	struct modifier_bits m;

	if (get_sr1_layout(flash, &sr1_layout))
		return -1;

	if (generic_range_table(flash, &r, &num_entries))
		return -1;

	/* modifier bits may be compared more than once, so get them here */
	if (use_s25f_modifier_bits(flash) && s25f_get_modifier_bits(flash, &m) < 0)
		return -1;

	sr1_bp = (sr1 >> sr1_layout.bp0_pos) & ((1 << sr1_layout.bp_bits) - 1);

	for (i = 0; i < num_entries; i++, r++) {
		if (use_s25f_modifier_bits(flash)) {
			if (memcmp(&m, &r->m, sizeof(m)))
				continue;
		}
		msg_cspew("comparing  0x%02x 0x%02x\n", sr1_bp, r->bp);
		if (sr1_bp == r->bp) {
			*start = r->range.start;
			*len = r->range.len;
			status_found = 1;
			break;
		}
	}

	if (!status_found) {
		msg_cerr("matching status not found\n");
		return -1;
	}

	return 0;
}

/* Given a [start, len], this function calls generic_range_to_status() to
 * convert it to flash-chip-specific range bits, then sets into status register.
 */
static int generic_set_range(const struct flashctx *flash,
                         unsigned int start, unsigned int len)
{
	uint8_t status, expected, check_mask;

	status = spi_read_status_register(flash);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, status);

	expected = status;	/* preserve non-bp bits */
	if (generic_range_to_status(flash, start, len, &expected, &check_mask))
		return -1;

	spi_write_status_register(flash, expected);

	status = spi_read_status_register(flash);
	msg_cdbg("%s: new status: 0x%02x\n", __func__, status);
	if ((status & check_mask) != (expected & check_mask)) {
		msg_cerr("expected=0x%02x, but actual=0x%02x. check mask=0x%02x\n",
		          expected, status, check_mask);
		return 1;
	}
	return 0;
}

/* Set/clear the status regsiter write protect bit in SR1. */
static int generic_set_srp0(const struct flashctx *flash, int enable)
{
	uint8_t status, expected, check_mask;
	struct status_register_layout sr1;

	if (get_sr1_layout(flash, &sr1))
		return -1;

	expected = spi_read_status_register(flash);
	msg_cdbg("%s: old status: 0x%02x\n", __func__, expected);

	if (enable)
		expected |= 1 << sr1.srp_pos;
	else
		expected &= ~(1 << sr1.srp_pos);

	spi_write_status_register(flash, expected);

	status = spi_read_status_register(flash);
	msg_cdbg("%s: new status: 0x%02x\n", __func__, status);

	check_mask = generic_get_status_check_mask(sr1);
	msg_cdbg("%s: check mask: 0x%02x\n", __func__, check_mask);
	if ((status & check_mask) != (expected & check_mask)) {
		msg_cerr("expected=0x%02x, but actual=0x%02x. check mask=0x%02x\n",
		          expected, status, check_mask);
		return -1;
	}

	return 0;
}

static int generic_enable_writeprotect(const struct flashctx *flash,
		enum wp_mode wp_mode)
{
	int ret;

	if (wp_mode != WP_MODE_HARDWARE) {
		msg_cerr("%s(): unsupported write-protect mode\n", __func__);
		return 1;
	}

	ret = generic_set_srp0(flash, 1);
	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);

	return ret;
}

static int generic_disable_writeprotect(const struct flashctx *flash)
{
	int ret;

	ret = generic_set_srp0(flash, 0);
	if (ret)
		msg_cerr("%s(): error=%d.\n", __func__, ret);

	return ret;
}

static int generic_list_ranges(const struct flashctx *flash)
{
	struct wp_range_descriptor *r;
	int i, num_entries;

	if (generic_range_table(flash, &r, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		msg_cinfo("start: 0x%06x, length: 0x%06x\n",
		          r->range.start, r->range.len);
		r++;
	}

	return 0;
}

static int wp_context_status(const struct flashctx *flash)
{
	uint8_t sr1;
	unsigned int start, len;
	int ret = 0;
	struct status_register_layout sr1_layout;
	int wp_en;

	if (get_sr1_layout(flash, &sr1_layout))
		return -1;

	sr1 = spi_read_status_register(flash);
	wp_en = (sr1 >> sr1_layout.srp_pos) & 1;

	msg_cinfo("WP: status: 0x%04x\n", sr1);
	msg_cinfo("WP: status.srp0: %x\n", wp_en);
	/* FIXME: SRP1 is not really generic, but we probably should print
	 * it anyway to have consistent output. #legacycruft */
	msg_cinfo("WP: status.srp1: %x\n", 0);
	msg_cinfo("WP: write protect is %s.\n",
		          wp_en ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (generic_status_to_range(flash, sr1, &start, &len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", start, len);
	}

	return ret;
}

struct wp wp_generic = {
	.list_ranges	= generic_list_ranges,
	.set_range	= generic_set_range,
	.enable		= generic_enable_writeprotect,
	.disable	= generic_disable_writeprotect,
	.wp_status	= wp_context_status,
};
