/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2007, 2008, 2009, 2010 Carl-Daniel Hailfinger
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
 * Contains the common SPI chip driver functions
 */

#include <stddef.h>
#include <string.h>
#include "flash.h"
#include "flashchips.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"
#include "spi4ba.h"

enum id_type {
	RDID,
	RDID4,
	REMS,
//	RES1,	/* TODO */
	RES2,
	NUM_ID_TYPES,
};

static struct {
	int is_cached;
	unsigned char bytes[4];		/* enough to hold largest ID type */
} id_cache[NUM_ID_TYPES];

void clear_spi_id_cache(void)
{
	memset(id_cache, 0, sizeof(id_cache));
	return;
}

static int spi_rdid(struct flashctx *flash, unsigned char *readarr, int bytes)
{
	static const unsigned char cmd[JEDEC_RDID_OUTSIZE] = { JEDEC_RDID };
	int ret;
	int i;

	ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	if (ret)
		return ret;
	msg_cspew("RDID returned");
	for (i = 0; i < bytes; i++)
		msg_cspew(" 0x%02x", readarr[i]);
	msg_cspew(". ");
	return 0;
}

static int spi_rems(struct flashctx *flash, unsigned char *readarr)
{
	unsigned char cmd[JEDEC_REMS_OUTSIZE] = { JEDEC_REMS, 0, 0, 0 };
	uint32_t readaddr;
	int ret;

	ret = spi_send_command(flash, sizeof(cmd), JEDEC_REMS_INSIZE, cmd, readarr);
	if (ret == SPI_INVALID_ADDRESS) {
		/* Find the lowest even address allowed for reads. */
		readaddr = (spi_get_valid_read_addr(flash) + 1) & ~1;
		cmd[1] = (readaddr >> 16) & 0xff,
		cmd[2] = (readaddr >> 8) & 0xff,
		cmd[3] = (readaddr >> 0) & 0xff,
		ret = spi_send_command(flash, sizeof(cmd), JEDEC_REMS_INSIZE, cmd, readarr);
	}
	if (ret)
		return ret;
	msg_cspew("REMS returned 0x%02x 0x%02x. ", readarr[0], readarr[1]);
	return 0;
}

static int spi_res(struct flashctx *flash, unsigned char *readarr, int bytes)
{
	unsigned char cmd[JEDEC_RES_OUTSIZE] = { JEDEC_RES, 0, 0, 0 };
	uint32_t readaddr;
	int ret;
	int i;

	ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	if (ret == SPI_INVALID_ADDRESS) {
		/* Find the lowest even address allowed for reads. */
		readaddr = (spi_get_valid_read_addr(flash) + 1) & ~1;
		cmd[1] = (readaddr >> 16) & 0xff,
		cmd[2] = (readaddr >> 8) & 0xff,
		cmd[3] = (readaddr >> 0) & 0xff,
		ret = spi_send_command(flash, sizeof(cmd), bytes, cmd, readarr);
	}
	if (ret)
		return ret;
	msg_cspew("RES returned");
	for (i = 0; i < bytes; i++)
		msg_cspew(" 0x%02x", readarr[i]);
	msg_cspew(". ");
	return 0;
}

int spi_write_enable(struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_WREN_OUTSIZE] = { JEDEC_WREN };
	int result;

	/* Send WREN (Write Enable) */
	result = spi_send_command(flash, sizeof(cmd), 0, cmd, NULL);

	if (result)
		msg_cerr("%s failed\n", __func__);

	return result;
}

int spi_write_disable(struct flashctx *flash)
{
	static const unsigned char cmd[JEDEC_WRDI_OUTSIZE] = { JEDEC_WRDI };

	/* Send WRDI (Write Disable) */
	return spi_send_command(flash, sizeof(cmd), 0, cmd, NULL);
}

static void rdid_get_ids(unsigned char *readarr,
		int bytes, uint32_t *id1, uint32_t *id2)
{
	if (!oddparity(readarr[0]))
		msg_cdbg("RDID byte 0 parity violation. ");

	/* Check if this is a continuation vendor ID.
	 * FIXME: Handle continuation device IDs.
	 */
	if (readarr[0] == 0x7f) {
		if (!oddparity(readarr[1]))
			msg_cdbg("RDID byte 1 parity violation. ");
		*id1 = (readarr[0] << 8) | readarr[1];
		*id2 = readarr[2];
		if (bytes > 3) {
			*id2 <<= 8;
			*id2 |= readarr[3];
		}
	} else {
		*id1 = readarr[0];
		*id2 = (readarr[1] << 8) | readarr[2];
	}
}

static int compare_id(struct flashctx *flash, uint32_t id1, uint32_t id2)
{
	msg_cdbg("id1 0x%02x, id2 0x%02x\n", id1, id2);

	if (id1 == flash->chip->manufacture_id && id2 == flash->chip->model_id)
		return 1;

	/* Test if this is a pure vendor match. */
	if (id1 == flash->chip->manufacture_id &&
	    GENERIC_DEVICE_ID == flash->chip->model_id)
		return 1;

	/* Test if there is any vendor ID. */
	if (GENERIC_MANUF_ID == flash->chip->manufacture_id &&
	    id1 != 0xff)
		return 1;

	return 0;
}

int probe_spi_rdid(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[RDID].is_cached) {
		if (spi_rdid(flash, id_cache[RDID].bytes, 3))
			return 0;
		id_cache[RDID].is_cached = 1;
	}

	rdid_get_ids(id_cache[RDID].bytes, 3, &id1, &id2);
	return compare_id(flash, id1, id2);
}

int probe_spi_rdid4(struct flashctx *flash)
{
	uint32_t id1, id2;

	/* Some SPI controllers do not support commands with writecnt=1 and
	 * readcnt=4.
	 */
	switch (spi_master->type) {
#if CONFIG_INTERNAL == 1
#if defined(__i386__) || defined(__x86_64__)
	case SPI_CONTROLLER_IT87XX:
	case SPI_CONTROLLER_WBSIO:
		msg_cinfo("4 byte RDID not supported on this SPI controller\n");
		break;
#endif
#endif
	default:
		break;
	}

	if (!id_cache[RDID4].is_cached) {
		if (spi_rdid(flash, id_cache[RDID4].bytes, 4))
			return 0;
		id_cache[RDID4].is_cached = 1;
	}

	rdid_get_ids(id_cache[RDID4].bytes, 4, &id1, &id2);
	return compare_id(flash, id1, id2);
}

int probe_spi_rems(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[REMS].is_cached) {
		if (spi_rems(flash, id_cache[REMS].bytes))
			return 0;
		id_cache[REMS].is_cached = 1;
	}

	id1 = id_cache[REMS].bytes[0];
	id2 = id_cache[REMS].bytes[1];
	return compare_id(flash, id1, id2);
}

int probe_spi_res1(struct flashctx *flash)
{
	static const unsigned char allff[] = {0xff, 0xff, 0xff};
	static const unsigned char all00[] = {0x00, 0x00, 0x00};
	unsigned char readarr[3];
	uint32_t id2;

	/* We only want one-byte RES if RDID and REMS are unusable. */

	/* Check if RDID is usable and does not return 0xff 0xff 0xff or
	 * 0x00 0x00 0x00. In that case, RES is pointless.
	 */
	if (!spi_rdid(flash, readarr, 3) && memcmp(readarr, allff, 3) &&
	    memcmp(readarr, all00, 3)) {
		msg_cdbg("Ignoring RES in favour of RDID.\n");
		return 0;
	}
	/* Check if REMS is usable and does not return 0xff 0xff or
	 * 0x00 0x00. In that case, RES is pointless.
	 */
	if (!spi_rems(flash, readarr) && memcmp(readarr, allff, JEDEC_REMS_INSIZE) &&
	    memcmp(readarr, all00, JEDEC_REMS_INSIZE)) {
		msg_cdbg("Ignoring RES in favour of REMS.\n");
		return 0;
	}

	if (spi_res(flash, readarr, 1)) {
		return 0;
	}

	id2 = readarr[0];

	msg_cdbg("%s: id 0x%x\n", __func__, id2);

	if (id2 != flash->chip->model_id)
		return 0;

	return 1;
}

int probe_spi_res2(struct flashctx *flash)
{
	uint32_t id1, id2;

	if (!id_cache[RES2].is_cached) {
		if (spi_res(flash, id_cache[RES2].bytes, 2))
			return 0;
		id_cache[RES2].is_cached = 1;
	}

	id1 = id_cache[RES2].bytes[0];
	id2 = id_cache[RES2].bytes[1];
	msg_cdbg("%s: id1 0x%x, id2 0x%x\n", __func__, id1, id2);

	if (id1 != flash->chip->manufacture_id || id2 != flash->chip->model_id)
		return 0;

	return 1;
}

/**
 * Execute WREN plus another one byte `op`, optionally poll WIP afterwards.
 *
 * @param flash       the flash chip's context
 * @param op          the operation to execute
 * @param poll_delay  interval in us for polling WIP, don't poll if zero
 * @return 0 on success, non-zero otherwise
 */
static int spi_simple_write_cmd(struct flashctx *const flash, const uint8_t op, const unsigned int poll_delay)
{
	struct spi_command cmds[] = {
	{
		.writecnt = 1,
		.writearr = (const unsigned char[]){ JEDEC_WREN },
	}, {
		.writecnt = 1,
		.writearr = (const unsigned char[]){ op },
	},
		NULL_SPI_CMD,
	};

	const int result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n", __func__);
		return result;
	}
	/* Wait until the Write-In-Progress bit is cleared.
	 * This usually takes 1-85 s, so wait in 1 s steps.
	 */
	/* FIXME: We can't tell if spi_read_status_register() failed. */
	/* FIXME: We don't time out. */
	while (poll_delay && spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(poll_delay);
	/* FIXME: Check the status register for errors. */

	return result;
}

int spi_chip_erase_60(struct flashctx *flash)
{
	/* This usually takes 1-85s, so wait in 1s steps. */
	return spi_simple_write_cmd(flash, 0x60, 1000 * 1000);
}

int spi_chip_erase_62(struct flashctx *flash)
{
	/* This usually takes 2-5s, so wait in 100ms steps. */
	return spi_simple_write_cmd(flash, 0x62, 100 * 1000);
}

int spi_chip_erase_c7(struct flashctx *flash)
{
	/* This usually takes 1-85s, so wait in 1s steps. */
	return spi_simple_write_cmd(flash, 0xc7, 1000 * 1000);
}

int spi_block_erase_52(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BE_52_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BE_52,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}
	/* Wait until the Write-In-Progress bit is cleared.
	 * This usually takes 100-4000 ms, so wait in 100 ms steps.
	 */
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(100 * 1000);
	/* FIXME: Check the status register for errors. */
	return 0;
}

/* Block size is usually
 * 64k for Macronix
 * 32k for SST
 * 4-32k non-uniform for EON
 */
int spi_block_erase_d8(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BE_D8_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BE_D8,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}
	/* Wait until the Write-In-Progress bit is cleared.
	 * This usually takes 100-4000 ms, so wait in 100 ms steps.
	 */
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(100 * 1000);
	/* FIXME: Check the status register for errors. */
	return 0;
}

/* Block size is usually
 * 4k for PMC
 */
int spi_block_erase_d7(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BE_D7_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BE_D7,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}
	/* Wait until the Write-In-Progress bit is cleared.
	 * This usually takes 100-4000 ms, so wait in 100 ms steps.
	 */
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(100 * 1000);
	/* FIXME: Check the status register for errors. */
	return 0;
}

/* Sector size is usually 4k, though Macronix eliteflash has 64k */
int spi_block_erase_20(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_SE_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_SE,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);

	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}
	/* Wait until the Write-In-Progress bit is cleared.
	 * This usually takes 15-800 ms, so wait in 10 ms steps.
	 */
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(10 * 1000);
	/* FIXME: Check the status register for errors. */
	return 0;
}

int spi_block_erase_60(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	if ((addr != 0) || (blocklen != flash->chip->total_size * 1024)) {
		msg_cerr("%s called with incorrect arguments\n",
			__func__);
		return -1;
	}
	return spi_chip_erase_60(flash);
}

int spi_block_erase_c7(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	if ((addr != 0) || (blocklen != flash->chip->total_size * 1024)) {
		msg_cerr("%s called with incorrect arguments\n",
			__func__);
		return -1;
	}
	return spi_chip_erase_c7(flash);
}

int spi_write_status_register_wren(const struct flashctx *flash, int status)
{
	int result;
	int i = 0;
	struct spi_command cmds[] = {
	{
	/* WRSR requires either EWSR or WREN depending on chip type. */
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_WRSR_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WRSR, (unsigned char) status },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n",
			__func__);
		/* No point in waiting for the command to complete if execution
		 * failed.
		 */
		return result;
	}
	/* WRSR performs a self-timed erase before the changes take effect.
	 * This may take 50-85 ms in most cases, and some chips apparently
	 * allow running RDSR only once. Therefore pick an initial delay of
	 * 100 ms, then wait in 10 ms steps until a total of 5 s have elapsed.
	 */
	programmer_delay(100 * 1000);
	while (spi_read_status_register(flash) & SPI_SR_WIP) {
		if (++i > 490) {
			msg_cerr("Error: WIP bit after WRSR never cleared\n");
			return TIMEOUT_ERROR;
		}
		programmer_delay(10 * 1000);
	}
	return 0;
}

int spi_byte_program(struct flashctx *flash, unsigned int addr, uint8_t databyte)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BYTE_PROGRAM_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BYTE_PROGRAM,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff),
					databyte
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
	}
	return result;
}

int spi_nbyte_program(struct flashctx *flash, unsigned int addr, const uint8_t *bytes, unsigned int len)
{
	int result;
	/* FIXME: Switch to malloc based on len unless that kills speed. */
	unsigned char cmd[JEDEC_BYTE_PROGRAM_OUTSIZE - 1 + 256] = {
		JEDEC_BYTE_PROGRAM,
		(addr >> 16) & 0xff,
		(addr >> 8) & 0xff,
		(addr >> 0) & 0xff,
	};
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BYTE_PROGRAM_OUTSIZE - 1 + len,
		.writearr	= cmd,
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	if (!len) {
		msg_cerr("%s called for zero-length write\n", __func__);
		return 1;
	}
	if (len > 256) {
		msg_cerr("%s called for too long a write\n", __func__);
		return 1;
	}

	memcpy(&cmd[4], bytes, len);

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		if (result != SPI_ACCESS_DENIED) {
			msg_cerr("%s failed during command execution at address 0x%x\n",
				__func__, addr);
		}
	}
	return result;
}

int spi_nbyte_read(struct flashctx *flash, unsigned int address, uint8_t *bytes, unsigned int len)
{
	const unsigned char cmd[JEDEC_READ_OUTSIZE] = {
		JEDEC_READ,
		(address >> 16) & 0xff,
		(address >> 8) & 0xff,
		(address >> 0) & 0xff,
	};

	/* Send Read */
	return spi_send_command(flash, sizeof(cmd), len, cmd, bytes);
}

/*
 * Read a part of the flash chip.
 * FIXME: Use the chunk code from Michael Karcher instead.
 * Each page is read separately in chunks with a maximum size of chunksize.
 */
int spi_read_chunked(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0, chunk_status = 0;
	unsigned int i, j, starthere, lenhere, toread;
	unsigned int page_size = flash->chip->page_size;

	/* Warning: This loop has a very unusual condition and body.
	 * The loop needs to go through each page with at least one affected
	 * byte. The lowest page number is (start / page_size) since that
	 * division rounds down. The highest page number we want is the page
	 * where the last byte of the range lives. That last byte has the
	 * address (start + len - 1), thus the highest page number is
	 * (start + len - 1) / page_size. Since we want to include that last
	 * page as well, the loop condition uses <=.
	 */
	for (i = start / page_size; i <= (start + len - 1) / page_size; i++) {
		/* Byte position of the first byte in the range in this page. */
		/* starthere is an offset to the base address of the chip. */
		starthere = max(start, i * page_size);
		/* Length of bytes in the range in this page. */
		lenhere = min(start + len, (i + 1) * page_size) - starthere;
		for (j = 0; j < lenhere; j += chunksize) {
			toread = min(chunksize, lenhere - j);
			chunk_status = (flash->chip->feature_bits & FEATURE_4BA_SUPPORT) == 0
				? spi_nbyte_read(flash, starthere + j, buf + starthere - start + j, toread)
				: flash->chip->four_bytes_addr_funcs.read_nbyte(flash, starthere + j,
					buf + starthere - start + j, toread);
			if (chunk_status) {
				if (ignore_error(chunk_status)) {
					/* fill this chunk with 0xff bytes and
					   let caller know about the error */
					memset(buf + starthere - start + j, 0xff, toread);
					rc = chunk_status;
					chunk_status = 0;
					continue;
				} else {
					rc = chunk_status;
					break;
				}
			}
		}
		if (chunk_status)
			break;
	}

	return rc;
}

/*
 * Read a part of the flash chip.
 * Ignore pages and read the data continuously, the only bound is the chunksize.
 */
int spi_read_unbound(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0;
	unsigned int i;

	for (i = start; i < (start + len); i += chunksize) {
		int chunk_status = 0;
		unsigned int toread = min(chunksize, start + len - i);

		if (flash->chip->feature_bits & FEATURE_4BA_SUPPORT) {
			chunk_status = flash->chip->four_bytes_addr_funcs.read_nbyte(
				flash, i, buf + (i - start), toread);
		} else {
			chunk_status = spi_nbyte_read(flash, i, buf + (i - start), toread);
		}

		if (chunk_status) {
			if (ignore_error(chunk_status)) {
				/* fill this chunk with 0xff bytes and
				   let caller know about the error */
				memset(buf + (i - start), 0xff, toread);
				rc = chunk_status;
				continue;
			} else {
				rc = chunk_status;
				break;
			}
		}
	}

	return rc;
}

/*
 * Write a part of the flash chip.
 * FIXME: Use the chunk code from Michael Karcher instead.
 * Each page is written separately in chunks with a maximum size of chunksize.
 */
int spi_write_chunked(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len, unsigned int chunksize)
{
	int rc = 0;
	unsigned int i, j, starthere, lenhere, towrite;
	/* FIXME: page_size is the wrong variable. We need max_writechunk_size
	 * in struct flashctx to do this properly. All chips using
	 * spi_chip_write_256 have page_size set to max_writechunk_size, so
	 * we're OK for now.
	 */
	unsigned int page_size = flash->chip->page_size;

	/* Warning: This loop has a very unusual condition and body.
	 * The loop needs to go through each page with at least one affected
	 * byte. The lowest page number is (start / page_size) since that
	 * division rounds down. The highest page number we want is the page
	 * where the last byte of the range lives. That last byte has the
	 * address (start + len - 1), thus the highest page number is
	 * (start + len - 1) / page_size. Since we want to include that last
	 * page as well, the loop condition uses <=.
	 */
	for (i = start / page_size; i <= (start + len - 1) / page_size; i++) {
		/* Byte position of the first byte in the range in this page. */
		/* starthere is an offset to the base address of the chip. */
		starthere = max(start, i * page_size);
		/* Length of bytes in the range in this page. */
		lenhere = min(start + len, (i + 1) * page_size) - starthere;
		for (j = 0; j < lenhere; j += chunksize) {
			towrite = min(chunksize, lenhere - j);
			rc = (flash->chip->feature_bits & FEATURE_4BA_SUPPORT) == 0
				? spi_nbyte_program(flash, starthere + j, buf + starthere - start + j, towrite)
				: flash->chip->four_bytes_addr_funcs.program_nbyte(flash, starthere + j,
					buf + starthere - start + j, towrite);
			if (rc)
				break;
			while (spi_read_status_register(flash) & SPI_SR_WIP)
				programmer_delay(10);
		}
		if (rc)
			break;
	}

	return rc;
}

/*
 * Program chip using byte programming. (SLOW!)
 * This is for chips which can only handle one byte writes
 * and for chips where memory mapped programming is impossible
 * (e.g. due to size constraints in IT87* for over 512 kB)
 */
/* real chunksize is 1, logical chunksize is 1 */
int spi_chip_write_1(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	unsigned int i;
	int result = 0;

	for (i = start; i < start + len; i++) {
		result = (flash->chip->feature_bits & FEATURE_4BA_SUPPORT) == 0
			? spi_byte_program(flash, i, buf[i - start])
			: flash->chip->four_bytes_addr_funcs.program_byte(flash, i, buf[i - start]);
		if (result)
			return 1;
		while (spi_read_status_register(flash) & SPI_SR_WIP)
			programmer_delay(10);
	}

	return 0;
}

int spi_aai_write(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	uint32_t pos = start;
	int result;
	unsigned char cmd[JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE] = {
		JEDEC_AAI_WORD_PROGRAM,
	};
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_AAI_WORD_PROGRAM_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_AAI_WORD_PROGRAM,
					(start >> 16) & 0xff,
					(start >> 8) & 0xff,
					(start & 0xff),
					buf[0],
					buf[1]
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	switch (spi_master->type) {
#if CONFIG_INTERNAL == 1
#if defined(__i386__) || defined(__x86_64__)
	case SPI_CONTROLLER_IT87XX:
	case SPI_CONTROLLER_WBSIO:
		msg_perr("%s: impossible with this SPI controller,"
				" degrading to byte program\n", __func__);
		return spi_chip_write_1(flash, buf, start, len);
#endif
#endif
	default:
		break;
	}

	/* The even start address and even length requirements can be either
	 * honored outside this function, or we can call spi_byte_program
	 * for the first and/or last byte and use AAI for the rest.
	 * FIXME: Move this to generic code.
	 */
	/* The data sheet requires a start address with the low bit cleared. */
	if (start % 2) {
		msg_cerr("%s: start address not even! Please report a bug at "
			 "flashrom@flashrom.org\n", __func__);
		if (spi_chip_write_1(flash, buf, start, start % 2))
			return SPI_GENERIC_ERROR;
		pos += start % 2;
		cmds[1].writearr = (const unsigned char[]){
					JEDEC_AAI_WORD_PROGRAM,
					(pos >> 16) & 0xff,
					(pos >> 8) & 0xff,
					(pos & 0xff),
					buf[pos - start],
					buf[pos - start + 1]
				};
		/* Do not return an error for now. */
		//return SPI_GENERIC_ERROR;
	}
	/* The data sheet requires total AAI write length to be even. */
	if (len % 2) {
		msg_cerr("%s: total write length not even! Please report a "
			 "bug at flashrom@flashrom.org\n", __func__);
		/* Do not return an error for now. */
		//return SPI_GENERIC_ERROR;
	}


	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during start command execution\n",
			 __func__);
		/* FIXME: Should we send WRDI here as well to make sure the chip
		 * is not in AAI mode?
		 */
		return result;
	}
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(10);

	/* We already wrote 2 bytes in the multicommand step. */
	pos += 2;

	/* Are there at least two more bytes to write? */
	while (pos < start + len - 1) {
		cmd[1] = buf[pos++ - start];
		cmd[2] = buf[pos++ - start];
		spi_send_command(flash, JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE, 0, cmd, NULL);
		while (spi_read_status_register(flash) & SPI_SR_WIP)
			programmer_delay(10);
	}

	/* Use WRDI to exit AAI mode. This needs to be done before issuing any
	 * other non-AAI command.
	 */
	spi_write_disable(flash);

	/* Write remaining byte (if any). */
	if (pos < start + len) {
		if (spi_chip_write_1(flash, buf + pos - start, pos, pos % 2))
			return SPI_GENERIC_ERROR;
		pos += pos % 2;
	}

	return 0;
}
