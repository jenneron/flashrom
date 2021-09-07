/*
 * This file is part of the flashrom project.
 *
 * Copyright 2021 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <string.h>
#include "flash.h"

/* Cros flashrom needs to be able to automatically identify the board's flash
 * without additional input. If multiple flashchip entries are considered to be
 * suitable matches, flashrom will fail to identify the chip and be unable to
 * perform the requested operations.
 *
 * This function allows flashrom to always pick a single flashchip entry, by
 * filtering out all unwanted duplicate flashchip entries and leaving only the
 * one we want to use.
 */
bool is_chipname_duplicate(const struct flashchip *chip)
{
	/* The "GD25B128B/GD25Q128B" and "GD25Q127C/GD25Q128C" chip entries
	 * have the same vendor and model IDs.
	 *
	 * Historically cros flashrom only had the "C" entry; an initial
	 * attempt to import the "B" from upstream entry resulted in flashrom
	 * being unable to identify the flash on Atlas and Nocturne boards,
	 * causing flashrom failures documented in b/168943314.
	 */
	if(!strcmp(chip->name, "GD25B128B/GD25Q128B")) return true;

	/* The "MX25L12805D" entry stops flashrom from identifying other
	 *  MX25L128... chips, block it. See: b/190574697.
	 */
	if(!strcmp(chip->name, "MX25L12805D")) return true;

	return false;
}
