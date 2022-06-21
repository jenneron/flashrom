/*
 * This file is part of the flashrom project.
 * It handles everything related to status registers of the JEDEC family 25.
 *
 * Copyright (C) 2007, 2008, 2009, 2010 Carl-Daniel Hailfinger
 * Copyright (C) 2008 coresystems GmbH
 * Copyright (C) 2008 Ronald Hoogenboom <ronald@zonnet.nl>
 * Copyright (C) 2012 Stefan Tauner
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

#include "flash.h"
#include "programmer.h"
#include "chipdrivers.h"
#include "spi.h"

static int opaque_restore_status(struct flashctx *flash, uint8_t status)
{
	msg_cdbg("restoring chip status (0x%02x)\n", status);
	return flash->mst->opaque.write_register(flash, STATUS1, status);
}

/* A common block protection disable that tries to unset the status register bits masked by 0x3C. */
int opaque_disable_blockprotect(struct flashctx *flash)
{
	uint8_t status;
	int result;

	const uint8_t bp_mask = 0x3C;
	const uint8_t lock_mask = 0;
	const uint8_t wp_mask = 0;
	const uint8_t unprotect_mask = 0xFF;

	int ret = flash->mst->opaque.read_register(flash, STATUS1, &status);
	if (ret)
		return ret;

	if ((status & bp_mask) == 0) {
		msg_cdbg2("Block protection is disabled.\n");
		return 0;
	}

	/* Restore status register content upon exit in finalize_flash_access(). */
	register_chip_restore(opaque_restore_status, flash, status);

	msg_cdbg("Some block protection in effect, disabling... ");
	if ((status & lock_mask) != 0) {
		msg_cdbg("\n\tNeed to disable the register lock first... ");
		if (wp_mask != 0 && (status & wp_mask) == 0) {
			msg_cerr("Hardware protection is active, disabling write protection is impossible.\n");
			return 1;
		}
		/* All bits except the register lock bit (often called SPRL, SRWD, WPEN) are readonly. */
		result = flash->mst->opaque.write_register(flash, STATUS1, status & ~lock_mask);
		if (result) {
			msg_cerr("Could not write status register 1.\n");
			return result;
		}

		ret = flash->mst->opaque.read_register(flash, STATUS1, &status);
		if (ret)
			return ret;

		if ((status & lock_mask) != 0) {
			msg_cerr("Unsetting lock bit(s) failed.\n");
			return 1;
		}
		msg_cdbg("done.\n");
	}
	/* Global unprotect. Make sure to mask the register lock bit as well. */
	result = flash->mst->opaque.write_register(flash, STATUS1,  status & ~(bp_mask | lock_mask) & unprotect_mask);
	if (result) {
		msg_cerr("Could not write status register 1.\n");
		return result;
	}

	ret = flash->mst->opaque.read_register(flash, STATUS1, &status);
	if (ret)
		return ret;

	if ((status & bp_mask) != 0) {
		msg_cerr("Block protection could not be disabled!\n");
		if (flash->chip->printlock)
			flash->chip->printlock(flash);
		return 1;
	}
	msg_cdbg("disabled.\n");
	return 0;
}
