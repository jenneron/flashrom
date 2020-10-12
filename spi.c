/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2007, 2008, 2009 Carl-Daniel Hailfinger
 * Copyright (C) 2008 coresystems GmbH
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
 * Contains the generic SPI framework
 */

#include <strings.h>
#include <string.h>
#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"

const struct spi_master spi_master_none = {
	.max_data_read = MAX_DATA_UNSPECIFIED,
	.max_data_write = MAX_DATA_UNSPECIFIED,
	.command = NULL,
	.multicommand = NULL,
	.read = NULL,
	.write_256 = NULL,
};

const struct spi_master *spi_master = &spi_master_none;

int spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
		const unsigned char *writearr, unsigned char *readarr)
{
	if (!spi_master->command) {
		msg_pdbg("%s called, but SPI is unsupported on this "
			 "hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	return spi_master->command(flash, writecnt, readcnt,
						      writearr, readarr);
}

int spi_send_multicommand(const struct flashctx *flash, struct spi_command *cmds)
{
	if (!spi_master->multicommand) {
		msg_pdbg("%s called, but SPI is unsupported on this "
			 "hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	return spi_master->multicommand(flash, cmds);
}

int default_spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
			     const unsigned char *writearr, unsigned char *readarr)
{
	struct spi_command cmd[] = {
	{
		.writecnt = writecnt,
		.readcnt = readcnt,
		.writearr = writearr,
		.readarr = readarr,
	}, {
		.writecnt = 0,
		.writearr = NULL,
		.readcnt = 0,
		.readarr = NULL,
	}};

	return spi_send_multicommand(flash, cmd);
}

int default_spi_send_multicommand(const struct flashctx *flash, struct spi_command *cmds)
{
	int result = 0;
	for (; (cmds->writecnt || cmds->readcnt) && !result; cmds++) {
		result = spi_send_command(flash, cmds->writecnt, cmds->readcnt,
					  cmds->writearr, cmds->readarr);
	}
	return result;
}

int default_spi_read(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len)
{
	unsigned int max_data = spi_master->max_data_read;
	int rc;
	if (max_data == MAX_DATA_UNSPECIFIED) {
		msg_perr("%s called, but SPI read chunk size not defined "
			 "on this hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}
	rc = spi_read_chunked(flash, buf, start, len, max_data);
	/* translate SPI-specific access denied error to generic error */
	if (rc == SPI_ACCESS_DENIED)
		rc = ACCESS_DENIED;
	return rc;
}

int default_spi_write_256(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	unsigned int max_data = spi_master->max_data_write;
	int rc;
	if (max_data == MAX_DATA_UNSPECIFIED) {
		msg_perr("%s called, but SPI write chunk size not defined "
			 "on this hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}
	rc = spi_write_chunked(flash, buf, start, len, max_data);
	/* translate SPI-specific access denied error to generic error */
	if (rc == SPI_ACCESS_DENIED)
		rc = ACCESS_DENIED;
	return rc;
}

int spi_chip_read(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len)
{
	if (!spi_master->read) {
		msg_perr("%s called, but SPI read is unsupported on this "
			 "hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	return spi_master->read(flash, buf, start, len);
}

/*
 * Program chip using page (256 bytes) programming.
 * Some SPI masters can't do this, they use single byte programming instead.
 * The redirect to single byte programming is achieved by setting
 * .write_256 = spi_chip_write_1
 */
/* real chunksize is up to 256, logical chunksize is 256 */
int spi_chip_write_256(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	if (!spi_master->write_256) {
		msg_perr("%s called, but SPI page write is unsupported on this "
			 "hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	return spi_master->write_256(flash, buf, start, len);
}

int spi_aai_write(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	return flash->mst->spi.write_aai(flash, buf, start, len);
}

int register_spi_master(const struct spi_master *mst)
{
	struct registered_master rmst;

	// TODO(quasisec): Kill off these global states.
	spi_master = mst;
	buses_supported |= BUS_SPI;

	rmst.buses_supported = BUS_SPI;
	rmst.spi = *mst;

	return register_master(&rmst);
}
