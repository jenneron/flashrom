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

static int range_table(const struct flashctx *flash,
                           struct wp_range_descriptor **descrs,
                           int *num_entries);

struct wp *get_wp_for_flashchip(const struct flashchip *chip) {
	// FIXME: The .wp field should be deleted from from struct flashchip
	// completly, but linux_mtd and cros_ec still assign their own values
	// to it. When they are cleaned up we can delete this.
	if(chip->wp) return chip->wp;

	switch (chip->manufacture_id) {
	case WINBOND_NEX_ID:
		switch(chip->model_id) {
		case WINBOND_NEX_W25Q128_V_M:
			return &wp_w25;
		case WINBOND_NEX_W25Q64_V:
                case WINBOND_NEX_W25Q64_W:
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
		case EON_EN25QH128:
			return &wp_w25;
		}
		break;
	case MACRONIX_ID:
		switch (chip->model_id) {
		case MACRONIX_MX25U6435E:
			return &wp_w25;
		}
		break;
	case ST_ID:
		switch(chip->model_id) {
		case XMC_XM25QH128C:
			return &wp_w25q;
		case XMC_XM25QH256C:
			return &wp_w25q_large;
		}
		break;
	case GIGADEVICE_ID:
		switch(chip->model_id) {
		case GIGADEVICE_GD25Q64:
		case GIGADEVICE_GD25LQ64:
		case GIGADEVICE_GD25Q128:
		case GIGADEVICE_GD25LQ128CD:
			return &wp_w25;
		case GIGADEVICE_GD25Q256D:
			return &wp_w25q_large;
		}
		break;
	case ATMEL_ID:
		switch(chip->model_id) {
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
	}


	return NULL;
}

int w25_range_to_status(const struct flashctx *flash,
                        unsigned int start, unsigned int len,
                        struct w25q_status *status)
{
	struct wp_range_descriptor *descrs;
	int i, range_found = 0;
	int num_entries;

	if (range_table(flash, &descrs, &num_entries))
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

	if (range_table(flash, &descrs, &num_entries))
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
static int w25_wp_status(const struct flashctx *flash,
		uint32_t *start, uint32_t *len, bool *wp_en)
{
	struct w25q_status status;
	int tmp;
	int ret = 0;

	memset(&status, 0, sizeof(status));
	tmp = spi_read_status_register(flash);
	memcpy(&status, &tmp, 1);
	msg_cinfo("WP: status: 0x%02x\n", tmp);
	msg_cinfo("WP: status.srp0: %x\n", status.srp0);
	msg_cinfo("WP: write protect is %s.\n",
	          status.srp0 ? "enabled" : "disabled");
	*wp_en = status.srp0;

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &status, start, len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", *start, *len);
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

	if (range_table(flash, &descrs, &num_entries))
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

	if (range_table(flash, &descrs, &num_entries))
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

static int w25q_large_wp_status(const struct flashctx *flash,
		uint32_t *start, uint32_t *len, bool *wp_en)
{
	struct w25q_status_large sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp[2];
	int ret = 0;

	memset(&sr1, 0, sizeof(sr1));
	tmp[0] = spi_read_status_register(flash);
	memcpy(&sr1, &tmp[0], 1);

	memset(&sr2, 0, sizeof(sr2));
	tmp[1] = w25q_read_status_register_2(flash);
	memcpy(&sr2, &tmp[1], 1);

	*wp_en = (sr1.srp0 || sr2.srp1);

	msg_cinfo("WP: status: 0x%02x%02x\n", tmp[1], tmp[0]);
	msg_cinfo("WP: status.srp0: %x\n", sr1.srp0);
	msg_cinfo("WP: status.srp1: %x\n", sr2.srp1);
	msg_cinfo("WP: write protect is %s.\n",
	          *wp_en ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_large_status_to_range(flash, &sr1, start, len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", *start, *len);
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

static int list_ranges(const struct flashctx *flash)
{
	struct wp_range_descriptor *descrs;
	int i, num_entries;

	if (range_table(flash, &descrs, &num_entries))
		return -1;

	for (i = 0; i < num_entries; i++) {
		msg_cinfo("start: 0x%06x, length: 0x%06x\n",
		          descrs[i].range.start,
		          descrs[i].range.len);
	}

	return 0;
}

static int w25q_wp_status(const struct flashctx *flash,
		uint32_t *start, uint32_t *len, bool *wp_en)
{
	struct w25q_status sr1;
	struct w25q_status_2 sr2;
	uint8_t tmp[2];
	int ret = 0;

	memset(&sr1, 0, sizeof(sr1));
	tmp[0] = spi_read_status_register(flash);
	memcpy(&sr1, &tmp[0], 1);

	memset(&sr2, 0, sizeof(sr2));
	tmp[1] = w25q_read_status_register_2(flash);
	memcpy(&sr2, &tmp[1], 1);

	*wp_en = (sr1.srp0 || sr2.srp1);

	msg_cinfo("WP: status: 0x%02x%02x\n", tmp[1], tmp[0]);
	msg_cinfo("WP: status.srp0: %x\n", sr1.srp0);
	msg_cinfo("WP: status.srp1: %x\n", sr2.srp1);
	msg_cinfo("WP: write protect is %s.\n",
	          *wp_en ? "enabled" : "disabled");

	msg_cinfo("WP: write protect range: ");
	if (w25_status_to_range(flash, &sr1, start, len)) {
		msg_cinfo("(cannot resolve the range)\n");
		ret = -1;
	} else {
		msg_cinfo("start=0x%08x, len=0x%08x\n", *start, *len);
	}

	return ret;
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
	.list_ranges	= list_ranges,
	.set_range	= w25_set_range,
	.enable		= w25_enable_writeprotect,
	.disable	= w25_disable_writeprotect,
	.wp_status	= w25_wp_status,

};

/* W25Q series has features such as a second status register and SFDP */
struct wp wp_w25q = {
	.list_ranges	= list_ranges,
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
	.list_ranges	= list_ranges,
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

/* Given a flash chip, this function returns its writeprotect info. */
static int range_table(const struct flashctx *flash,
                           struct wp_range_descriptor **descrs,
                           int *num_entries)
{
	*num_entries = 0;

	switch (flash->chip->manufacture_id) {
	case ATMEL_ID:
		switch(flash->chip->model_id) {
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
	case WINBOND_NEX_ID:
		switch(flash->chip->model_id) {
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
		default:
			msg_cerr("%s():%d: EON flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
				 flash->chip->model_id);
			return -1;
		}
		break;

	case GIGADEVICE_ID:
		switch(flash->chip->model_id) {

		case GIGADEVICE_GD25Q64:
		case GIGADEVICE_GD25LQ64:
			*descrs = gd25q64_ranges;
			*num_entries = ARRAY_SIZE(gd25q64_ranges);
			break;
		case GIGADEVICE_GD25Q128:
		case GIGADEVICE_GD25LQ128CD:
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
	case MACRONIX_ID:
		switch (flash->chip->model_id) {
		case MACRONIX_MX25U6435E:
			*descrs = mx25u6435e_ranges;
			*num_entries = ARRAY_SIZE(mx25u6435e_ranges);
			break;
		default:
			msg_cerr("%s():%d: MXIC flash chip mismatch (0x%04x)"
			         ", aborting\n", __func__, __LINE__,
			         flash->chip->model_id);
			return -1;
		}
		break;
	case ST_ID:
		switch(flash->chip->model_id) {
		case XMC_XM25QH128C:
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
		case XMC_XM25QH256C:
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
			msg_cerr("%s() %d: Micron flash chip mismatch"
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
