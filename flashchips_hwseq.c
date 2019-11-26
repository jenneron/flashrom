/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2004 Tyan Corp
 * Copyright (C) 2005-2008 coresystems GmbH <stepan@openbios.org>
 * Copyright (C) 2006-2009 Carl-Daniel Hailfinger
 * Copyright (C) 2009 Sean Nelson <audiohacked@gmail.com>
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
 */

#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "writeprotect.h"

/* Generic flashchip struct for platforms that use Intel hardware sequencing. */
const struct flashchip flashchips_hwseq[] = {
	{
		.vendor		= "Generic",
		.name		= "HWSEQ chip",
		.bustype	= BUS_PROG,
		/* probe is assumed to work, rest will be filled in by probe */
		.tested		= TEST_OK_PREW,
		.probe		= probe_opaque,
		/* eraseblock sizes will be set by the probing function */
		.block_erasers	=
		{
			{
				.block_erase = erase_opaque,
			}
		},
		.write		= write_opaque,
		.read		= read_opaque,
		.read_status	= read_status_opaque,
		.write_status	= write_status_opaque,
		.check_access	= check_access_opaque,
		.wp		= &wp_w25,
		.unlock	= &spi_disable_blockprotect,
	},

	{NULL}
};

int flash_erase_value(struct flashctx *flash)
{
	return flash->chip->feature_bits & FEATURE_ERASED_ZERO ? 0 : 0xff;
}

int flash_unerased_value(struct flashctx *flash)
{
	return flash->chip->feature_bits & FEATURE_ERASED_ZERO ? 0xff : 0;
}

const struct flashchip *flash_id_to_entry(uint32_t mfg_id, uint32_t model_id)
{
	const struct flashchip *chip;

	for (chip = &flashchips[0]; chip->vendor; chip++) {
		if ((chip->manufacture_id == mfg_id) &&
			(chip->model_id == model_id))
			return chip;
	}

	return NULL;
}

struct voltage_range voltage_ranges[NUM_VOLTAGE_RANGES];

static int compar(const void *_x, const void *_y)
{
	const struct voltage_range *x = _x;
	const struct voltage_range *y = _y;

	/*
	 * qsort() places entries in ascending order. We will sort by minimum
	 * voltage primarily and max voltage secondarily, and move empty sets
	 * to the end of array.
	 */
	if (x->min == 0)
		return 1;
	if (y->min == 0)
		return -1;

	if (x->min < y->min)
		return -1;
	if (x->min > y->min)
		return 1;

	if (x->min == y->min) {
		if (x->max < y->max)
			return -1;
		if (x->max > y->max)
			return 1;
	}

	return 0;
}

int flash_supported_voltage_ranges(enum chipbustype bus)
{
	int i;
	int unique_ranges = 0;

	/* clear array in case user calls this function multiple times */
	memset(voltage_ranges, 0, sizeof(voltage_ranges));

	for (i = 0; i < flashchips_size; i++) {
		int j;
		int match_found = 0;

		if (unique_ranges >= NUM_VOLTAGE_RANGES) {
			msg_cerr("Increase NUM_VOLTAGE_RANGES.\n");
			return -1;
		}

		if (!(flashchips[i].bustype & bus))
			continue;

		for (j = 0; j < NUM_VOLTAGE_RANGES; j++) {
			if ((flashchips[i].voltage.min == voltage_ranges[j].min) &&
				(flashchips[i].voltage.max == voltage_ranges[j].max))
				match_found |= 1;

			if (!voltage_ranges[j].min && !voltage_ranges[j].max)
				break;
		}

		if (!match_found) {
			voltage_ranges[j] = flashchips[i].voltage;
			unique_ranges++;
		}
	}

	qsort(&voltage_ranges[0], NUM_VOLTAGE_RANGES,
			sizeof(voltage_ranges[0]), compar);

	for (i = 0; i < NUM_VOLTAGE_RANGES; i++) {
		msg_cspew("%s: voltage_range[%d]: { %u, %u }\n",
			__func__, i, voltage_ranges[i].min, voltage_ranges[i].max);
	}

	return unique_ranges;
}
