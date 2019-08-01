/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2011 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
 * Contains the opaque programmer framework.
 * An opaque programmer is a programmer which does not provide direct access
 * to the flash chip and which abstracts all flash chip properties into a
 * programmer specific interface.
 */

#include <stdint.h>
#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "programmer.h"

struct opaque_master opaque_master_none = {
	.max_data_read = MAX_DATA_UNSPECIFIED,
	.max_data_write = MAX_DATA_UNSPECIFIED,
	.probe = NULL,
	.read = NULL,
	.write = NULL,
	.read_status = NULL,
	.write_status = NULL,
	.erase = NULL,
	.check_access = NULL,
};

struct opaque_master *opaque_master = &opaque_master_none;

int probe_opaque(struct flashctx *flash)
{
	if (!opaque_master->probe) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 0;
	}

	return opaque_master->probe(flash);
}

int read_opaque(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len)
{
	if (!opaque_master->read) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 1;
	}
	return opaque_master->read(flash, buf, start, len);
}

int write_opaque(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	if (!opaque_master->write) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 1;
	}
	return opaque_master->write(flash, buf, start, len);
}

int erase_opaque(struct flashctx *flash, unsigned int blockaddr, unsigned int blocklen)
{
	if (!opaque_master->erase) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 1;
	}
	return opaque_master->erase(flash, blockaddr, blocklen);
}

uint8_t read_status_opaque(const struct flashctx *flash)
{
	if (!opaque_master->read_status) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 1;
	}
	return opaque_master->read_status(flash);
}

int write_status_opaque(const struct flashctx *flash, int status)
{
	if (!opaque_master->write_status) {
		msg_perr("%s called before register_opaque_master. "
			 "Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return 1;
	}
	return opaque_master->write_status(flash, status);
}

int check_access_opaque(const struct flashctx *flash, unsigned int start, unsigned int len, int rw)
{
	if (opaque_master->check_access)
		return opaque_master->check_access(flash, start, len, rw);
	return 1;
}

void register_opaque_master(struct opaque_master *pgm)
{
	if (!pgm->probe || !pgm->read || !pgm->write || !pgm->erase) {
		msg_perr("%s called with one of probe/read/write/erase being "
			 "NULL. Please report a bug at flashrom@flashrom.org\n",
			 __func__);
		return;
	}
	opaque_master = pgm;
	buses_supported |= BUS_PROG;
}
