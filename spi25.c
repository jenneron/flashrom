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
#include <stdbool.h>
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

static int spi_poll_wip(struct flashctx *const flash, const unsigned int poll_delay)
{
	/* FIXME: We can't tell if spi_read_status_register() failed. */
	/* FIXME: We don't time out. */
	while (spi_read_status_register(flash) & SPI_SR_WIP)
		programmer_delay(poll_delay);
	/* FIXME: Check the status register for errors. */
	return 0;
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

	const int status = poll_delay ? spi_poll_wip(flash, poll_delay) : 0;

	return result ? result : status;
}

static int spi_set_extended_address(struct flashctx *const flash, const uint8_t addr_high)
{
	if (flash->address_high_byte != addr_high &&
	    spi_write_extended_address_register(flash, addr_high))
		return -1;
	flash->address_high_byte = addr_high;
	return 0;
}

static int spi_prepare_address(struct flashctx *const flash, uint8_t cmd_buf[],
			       const bool native_4ba, const unsigned int addr)
{
	if (native_4ba || flash->in_4ba_mode) {
		cmd_buf[1] = (addr >> 24) & 0xff;
		cmd_buf[2] = (addr >> 16) & 0xff;
		cmd_buf[3] = (addr >>  8) & 0xff;
		cmd_buf[4] = (addr >>  0) & 0xff;
		return 4;
	} else {
		if (flash->chip->feature_bits & FEATURE_4BA_EXT_ADDR) {
			if (spi_set_extended_address(flash, addr >> 24))
				return -1;
		} else {
			if (addr >> 24)
				return -1;
		}
		cmd_buf[1] = (addr >> 16) & 0xff;
		cmd_buf[2] = (addr >>  8) & 0xff;
		cmd_buf[3] = (addr >>  0) & 0xff;
		return 3;
	}
}

/**
 * Execute WREN plus another `op` that takes an address and
 * optional data, poll WIP afterwards.
 *
 * @param flash       the flash chip's context
 * @param op          the operation to execute
 * @param native_4ba  where `op` always takes a 4-byte address
 * @param addr        the address parameter to `op`
 * @param out_bytes   bytes to send after the address,
 *                    may be NULL if and only if `out_bytes` is 0
 * @param out_bytes   number of bytes to send, 256 at most, may be zero
 * @param poll_delay  interval in us for polling WIP
 * @return 0 on success, non-zero otherwise
 */
static int spi_write_cmd(struct flashctx *const flash, const uint8_t op,
			 const bool native_4ba, const unsigned int addr,
			 const uint8_t *const out_bytes, const size_t out_len,
			 const unsigned int poll_delay)
{
	uint8_t cmd[1 + JEDEC_MAX_ADDR_LEN + 256];
	struct spi_command cmds[] = {
	{
		.writecnt = 1,
		.writearr = (const unsigned char[]){ JEDEC_WREN },
	}, {
		.writearr = cmd,
	},
		NULL_SPI_CMD,
	};

	cmd[0] = op;
	const int addr_len = spi_prepare_address(flash, cmd, native_4ba, addr);
	if (addr_len < 0)
		return 1;

	if (1 + addr_len + out_len > sizeof(cmd)) {
		msg_cerr("%s called for too long a write\n", __func__);
		return 1;
	}

	memcpy(cmd + 1 + addr_len, out_bytes, out_len);
	cmds[1].writecnt = 1 + addr_len + out_len;

	const int result = spi_send_multicommand(flash, cmds);
	if (result)
		msg_cerr("%s failed during command execution at address 0x%x\n", __func__, addr);

	const int status = spi_poll_wip(flash, poll_delay);

	return result ? result : status;
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
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0x52, false, addr, NULL, 0, 100 * 1000);
}

/* Block size is usually
 * 32M (one die) for Micron
 */
int spi_block_erase_c4(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 240-480s, so wait in 500ms steps. */
	return spi_write_cmd(flash, 0xc4, false, addr, NULL, 0, 500 * 1000);
}

/* Block size is usually
 * 64k for Macronix
 * 32k for SST
 * 4-32k non-uniform for EON
 */
int spi_block_erase_d8(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0xd8, false, addr, NULL, 0, 100 * 1000);
}

/* Block size is usually
 * 4k for PMC
 */
int spi_block_erase_d7(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0xd7, false, addr, NULL, 0, 100 * 1000);
}

/* Page erase (usually 256B blocks) */
int spi_block_erase_db(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This takes up to 20ms usually (on worn out devices
	   up to the 0.5s range), so wait in 1ms steps. */
	return spi_write_cmd(flash, 0xdb, false, addr, NULL, 0, 1 * 1000);
}

/* Sector size is usually 4k, though Macronix eliteflash has 64k */
int spi_block_erase_20(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 15-800ms, so wait in 10ms steps. */
	return spi_write_cmd(flash, 0x20, false, addr, NULL, 0, 10 * 1000);
}

int spi_block_erase_50(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 10ms, so wait in 1ms steps. */
	return spi_write_cmd(flash, 0x50, false, addr, NULL, 0, 1 * 1000);
}

int spi_block_erase_81(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 8ms, so wait in 1ms steps. */
	return spi_write_cmd(flash, 0x81, false, addr, NULL, 0, 1 * 1000);
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

/* Erase 4 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes)
   JEDEC_SE_4BA (21h) instruction is new for 4-bytes addressing flash chips.
   The presence of this instruction for an exact chip should be checked
   by its datasheet or from SFDP 4-Bytes Address Instruction Table (JESD216B). */
int spi_block_erase_21(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 15-800ms, so wait in 10ms steps. */
	return spi_write_cmd(flash, 0x21, true, addr, NULL, 0, 10 * 1000);
}

/* Erase 32 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes)
   JEDEC_BE_5C_4BA (5Ch) instruction is new for 4-bytes addressing flash chips.
   The presence of this instruction for an exact chip should be checked
   by its datasheet or from SFDP 4-Bytes Address Instruction Table (JESD216B). */
int spi_block_erase_5c(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0x5c, true, addr, NULL, 0, 100 * 1000);
}

/* Erase 64 KB of flash with 4-bytes address from ANY mode (3-bytes or 4-bytes)
   JEDEC_BE_DC_4BA (DCh) instruction is new for 4-bytes addressing flash chips.
   The presence of this instruction for an exact chip should be checked
   by its datasheet or from SFDP 4-Bytes Address Instruction Table (JESD216B). */
int spi_block_erase_dc(struct flashctx *flash, unsigned int addr, unsigned int blocklen)
{
	/* This usually takes 100-4000ms, so wait in 100ms steps. */
	return spi_write_cmd(flash, 0xdc, true, addr, NULL, 0, 100 * 1000);
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

static int spi_nbyte_program(struct flashctx *flash, unsigned int addr, const uint8_t *bytes, unsigned int len)
{
	const bool native_4ba = !!(flash->chip->feature_bits & FEATURE_4BA_WRITE);
	const uint8_t op = native_4ba ? JEDEC_BYTE_PROGRAM_4BA : JEDEC_BYTE_PROGRAM;
	return spi_write_cmd(flash, op, native_4ba, addr, bytes, len, 10);
}

int spi_nbyte_read(struct flashctx *flash, unsigned int address, uint8_t *bytes, unsigned int len)
{
	const bool native_4ba = !!(flash->chip->feature_bits & FEATURE_4BA_READ);
	uint8_t cmd[1 + JEDEC_MAX_ADDR_LEN] = { native_4ba ? JEDEC_READ_4BA : JEDEC_READ, };

	const int addr_len = spi_prepare_address(flash, cmd, native_4ba, address);
	if (addr_len < 0)
		return 1;

	/* Send Read */
	return spi_send_command(flash, 1 + addr_len, len, cmd, bytes);
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
			chunk_status = spi_nbyte_read(flash, starthere + j, buf + starthere - start + j, toread);
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

		chunk_status = spi_nbyte_read(flash, i, buf + (i - start), toread);
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
			int rc;
			towrite = min(chunksize, lenhere - j);
			rc = spi_nbyte_program(flash, starthere + j, buf + starthere - start + j, towrite);
			if (rc)
				return rc;
		}
	}

	return 0;
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

	for (i = start; i < start + len; i++) {
		if (spi_nbyte_program(flash, i, buf + i - start, 1))
			return 1;
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

	result = spi_write_cmd(flash, JEDEC_AAI_WORD_PROGRAM, false, start, buf + pos - start, 2, 10);
	if (result)
		goto bailout;

	/* We already wrote 2 bytes in the multicommand step. */
	pos += 2;

	/* Are there at least two more bytes to write? */
	while (pos < start + len - 1) {
		cmd[1] = buf[pos++ - start];
		cmd[2] = buf[pos++ - start];
		result = spi_send_command(flash, JEDEC_AAI_WORD_PROGRAM_CONT_OUTSIZE, 0, cmd, NULL);
		if (result) {
			msg_cerr("%s failed during followup AAI command execution: %d\n", __func__, result);
			goto bailout;
		}
		if (spi_poll_wip(flash, 10))
			goto bailout;
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

bailout:
	spi_write_disable(flash);
	return SPI_GENERIC_ERROR;
}
