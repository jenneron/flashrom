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
	.type = SPI_CONTROLLER_NONE,
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
	if (flash->chip->feature_bits & FEATURE_UNBOUND_READ)
		rc = spi_read_unbound(flash, buf, start, len, max_data);
	else
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
	unsigned int addrbase = 0;
	if (!spi_master->read) {
		msg_perr("%s called, but SPI read is unsupported on this "
			 "hardware. Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	/* Check if the chip fits between lowest valid and highest possible
	 * address. Highest possible address with the current SPI implementation
	 * means 0xffffff, the highest unsigned 24bit number.
	 */
	addrbase = spi_get_valid_read_addr(flash);
	/* Show flash chip size warning if flash chip doesn't support
	   4-Bytes Addressing mode and last address excedes 24 bits */
	if (!(flash->chip->feature_bits & FEATURE_4BA_ENTER_WREN) &&
	    addrbase + flash->chip->total_size * 1024 > (1 << 24)) {
		msg_perr("Flash chip size exceeds the allowed access window. ");
		msg_perr("Read will probably fail.\n");
		/* Try to get the best alignment subject to constraints. */
		addrbase = (1 << 24) - flash->chip->total_size * 1024;
	}
	/* Check if alignment is native (at least the largest power of two which
	 * is a factor of the mapped size of the chip).
	 */
	if (ffs(flash->chip->total_size * 1024) > (ffs(addrbase) ? : 33)) {
		msg_perr("Flash chip is not aligned natively in the allowed "
			 "access window.\n");
		msg_perr("Read will probably return garbage.\n");
	}
	return spi_master->read(flash, buf, addrbase + start, len);
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

/*
 * Get the lowest allowed address for read accesses. This often happens to
 * be the lowest allowed address for all commands which take an address.
 * This is a programmer limitation.
 */
uint32_t spi_get_valid_read_addr(struct flashctx *flash)
{
	switch (spi_master->type) {
#if CONFIG_INTERNAL == 1
#if defined(__i386__) || defined(__x86_64__)
	case SPI_CONTROLLER_ICH7:
		/* Return BBAR for ICH chipsets. */
		return ichspi_bbar;
#endif
#endif
	default:
		return 0;
	}
}

void register_spi_master(const struct spi_master *pgm)
{
	spi_master = pgm;
	buses_supported |= BUS_SPI;
}
