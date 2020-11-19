/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Nuvoton Technology Corporation. or the names of
 * contributors or licensors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * NUVOTON TECHNOLOGY CORPORATION. ("NUVOTON") AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * SUN OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * This is an UNOFFICIAL patch for the Nuvoton WPCE775x/NPCE781x. It was tested
 * for a specific hardware and firmware configuration and should be considered
 * unreliable. Please see the following URL for Nuvoton's authoritative,
 * officially supported flash update utility:
 * http://sourceforge.net/projects/nuvflashupdate/
 */

#if defined(__i386__) || defined(__x86_64__)
#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include "flash.h"
#include "chipdrivers.h"
#include "flashchips.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

/**
 *  Definition of WPCE775X WCB (Write Command Buffer), as known as Shared Access
 *  Window 2.
 *
 *  The document name is "WPCE775X Software User Guide Revision 1.2".
 *
 *  Assume the host is little endian.
 */
struct __attribute__((packed)) wpce775x_wcb {
	/* Byte 0: semaphore byte */
	uint8_t exe:1;  /* Bit0-RW- set by host. means wcb is ready to execute.
	                         should be cleared by host after RDY=1. */
	uint8_t resv0_41:4;
	uint8_t pcp:1;  /* Bit5-RO- set by EPCE775x. means preparation operations for
	                         flash update process is complete. */
	uint8_t err:1;  /* Bit6-RO- set by EPCE775x. means an error occurs. */
	uint8_t rdy:1;  /* Bit7-RO- set by EPCE775x. means operation is completed. */

	/* Byte 1-2: reserved */
	uint8_t byte1;
	uint8_t byte2;

	/* Byte 3: command code */
	uint8_t code;

	/* Byte 4-15: command field */
	uint8_t field[12];
};

/* The physical address of WCB -- Shared Access Window 2. */
static chipaddr wcb_physical_address;

/* The virtual address of WCB -- Shared Access Window 2. */
static volatile struct wpce775x_wcb *volatile wcb;

/* count of entering flash update mode */
static int in_flash_update_mode;

static int firmware_changed;

/*
 * Bytes 0x4-0xf of init_flash command. These represent opcodes and various
 * parameters the WPCE775x will use when communicating with the SPI flash
 * device. DO NOT RE-ORDER THIS STRUCTURE.
 */
struct wpce775x_initflash_cfg {
	uint8_t read_device_id;		/* Byte 0x04. Ex: JEDEC_RDID */
	uint8_t write_status_enable;	/* Byte 0x05. Ex: JEDEC_EWSR */
	uint8_t write_enable;		/* Byte 0x06. Ex: JEDEC_WREN */
	uint8_t read_status_register;	/* Byte 0x07. Ex: JEDEC_RDSR */
	uint8_t write_status_register;	/* Byte 0x08. Ex: JEDEC_WRSR */
	uint8_t flash_program;		/* Byte 0x09. Ex: JEDEC_BYTE_PROGRAM */

	/* Byte 0x0A. Ex: sector/block/chip erase opcode */
	uint8_t block_erase;

	uint8_t status_busy_mask;	/* Byte B: bit position of BUSY bit */

	/* Byte 0x0C: value to remove write protection */
	uint8_t status_reg_value;

	/* Byte 0x0D: Number of bytes to program in each write transaction. */
	uint8_t program_unit_size;

	uint8_t page_size;		/* Byte 0x0E: 2^n bytes */

	/*
	 * Byte 0x0F: Method to read device ID. 0x47 will cause ID bytes to be
	 * read immediately after read_device_id command is issued. Otherwise,
	 * 3 dummy address bytes are sent after the read_device_id code.
	 */
	uint8_t read_device_id_type;
} __attribute__((packed));

/*
 * The WPCE775x can use init_flash multiple times during an update. We'll use
 * this ability primarily for changing write protection bits.
 */
static struct wpce775x_initflash_cfg *initflash_cfg;

/* SuperI/O related definitions and functions. */
/* Strapping options */
#define NUVOTON_SIO_PORT1	0x2e	/* No pull-down resistor */
#define NUVOTON_SIO_PORT2	0x164e	/* Pull-down resistor on BADDR0 */
/* Note: There's another funky state that we won't worry about right now */

/* SuperI/O Config */
#define NUVOTON_SIOCFG_LDN	0x07	/* LDN Bank Selector */
#define NUVOTON_SIOCFG_SID	0x20	/* SuperI/O ID */
#define NUVOTON_SIOCFG_SRID	0x27	/* SuperI/O Revision ID */
#define NUVOTON_LDN_SHM         0x0f    /* LDN of SHM module */

/* WPCE775x shared memory config registers (LDN 0x0f) */
#define WPCE775X_SHM_BASE_MSB	0x60
#define WPCE775X_SHM_BASE_LSB	0x61
#define WPCE775X_SHM_CFG	0xf0
#define WPCE775X_SHM_CFG_BIOS_FWH_EN	(1 << 3)
#define WPCE775X_SHM_CFG_FLASH_ACC_EN	(1 << 2)
#define WPCE775X_SHM_CFG_BIOS_EXT_EN	(1 << 1)
#define WPCE775X_SHM_CFG_BIOS_LPC_EN	(1 << 0)
#define WPCE775X_WIN_CFG		0xf1	/* window config */
#define WPCE775X_WIN_CFG_SHWIN_ACC	(1 << 6)

/* Shared access window 2 bar address registers */
#define WPCE775X_SHAW2BA_0	0xf8
#define WPCE775X_SHAW2BA_1	0xf9
#define WPCE775X_SHAW2BA_2	0xfa
#define WPCE775X_SHAW2BA_3	0xfb

/* Read/write buffer size */
#define WPCE775X_MAX_WRITE_SIZE	 8
#define WPCE775X_MAX_READ_SIZE	12

/** probe for super i/o index
 *  @returns 0 to indicate success, <0 to indicate error
 */
static int nuvoton_get_sio_index(uint16_t *port)
{
	uint16_t ports[] = { NUVOTON_SIO_PORT2,
	                     NUVOTON_SIO_PORT1,
	};
	static uint16_t port_internal, port_found = 0;

	if (port_found) {
		*port = port_internal;
		return 0;
	}

	if (rget_io_perms())
		return 1;

	for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
		uint8_t sid = sio_read(ports[i], NUVOTON_SIOCFG_SID);

		if (sid == 0xfc) {  /* Family ID */
			port_internal = ports[i];
			port_found = 1;
			break;
		}
	}

	if (!port_found) {
		msg_cdbg("\nfailed to obtain super i/o index\n");
		return -1;
	}

	msg_cdbg("\nsuper i/o index = 0x%04x\n", port_internal);
	*port = port_internal;
	return 0;
}

/** Call superio to get pre-configured WCB address.
 *  Read LDN 0x0f (SHM) idx:f8-fb (little-endian).
 */
static int get_shaw2ba(chipaddr *shaw2ba)
{
	uint16_t idx;
	uint8_t org_ldn;
	uint8_t win_cfg;
	uint8_t shm_cfg;

	if (nuvoton_get_sio_index(&idx) < 0)
		return -1;

	org_ldn = sio_read(idx, NUVOTON_SIOCFG_LDN);
	sio_write(idx, NUVOTON_SIOCFG_LDN, NUVOTON_LDN_SHM);

	/*
	 * To obtain shared access window 2 base address, we must OR the base
	 * address bytes, where SHAW2BA_0 is least significant and SHAW2BA_3
	 * most significant.
	 */
	*shaw2ba = sio_read(idx, WPCE775X_SHAW2BA_0) |
	           ((chipaddr)sio_read(idx, WPCE775X_SHAW2BA_1) << 8) |
	           ((chipaddr)sio_read(idx, WPCE775X_SHAW2BA_2) << 16) |
	           ((chipaddr)sio_read(idx, WPCE775X_SHAW2BA_3) << 24);

	/*
	 * If SHWIN_ACC is cleared, then we're using LPC memory access
	 * and SHAW2BA_3-0 indicate bits 31-0. If SHWIN_ACC is set, then
	 * bits 7-4 of SHAW2BA_3 are ignored and bits 31-28 are indicated
	 * by the idsel nibble. (See table 25 "supported host address ranges"
	 * for more details)
	 */
	win_cfg = sio_read(idx, WPCE775X_WIN_CFG);
	if (win_cfg & WPCE775X_WIN_CFG_SHWIN_ACC) {
		uint8_t idsel;

		/* Make sure shared BIOS memory is enabled */
		shm_cfg = sio_read(idx, WPCE775X_SHM_CFG);
		if ((shm_cfg & WPCE775X_SHM_CFG_BIOS_FWH_EN))
			idsel = 0xf;
		else {
			msg_cdbg("Shared BIOS memory is diabled.\n");
			msg_cdbg("Please check SHM_CFG:BIOS_FWH_EN.\n");
	                goto error;
		}

		*shaw2ba &= 0x0fffffff;
		*shaw2ba |= (chipaddr)idsel << 28;
	}

	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);
	return 0;
error:
	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);
	return -1;
}

/* Call superio to get pre-configured fwh_id.
 * Read LDN 0x0f (SHM) idx:f0.
 */
static int get_fwh_id(uint8_t *fwh_id)
{
	uint16_t idx;
	uint8_t org_ldn;

	if (nuvoton_get_sio_index(&idx) < 0)
		return -1;

	org_ldn = sio_read(idx, NUVOTON_SIOCFG_LDN);
	sio_write(idx, NUVOTON_SIOCFG_LDN, NUVOTON_LDN_SHM);
	*fwh_id = sio_read(idx, WPCE775X_SHM_CFG);
	sio_write(idx, NUVOTON_SIOCFG_LDN, org_ldn);

	return 0;
}

/** helper function to make sure the exe bit is 0 (no one is using EC).
 *  @return 1 for error; 0 for success.
 */
static int assert_ec_is_free(void)
{
	if (wcb->exe)
		msg_perr("ASSERT(wcb->exe==0), entering busy loop.\n");
	while(wcb->exe);
	return 0;
}

/** Trigger EXE bit, and block until operation completes.
 *  @return 1 for error; and 0 for success.
 */
static int blocked_exec(void)
{
	struct timeval begin, now;
	int timeout;  /* not zero if timeout occurs */
	int err;

	assert(wcb->rdy==0);

	/* raise EXE bit, and wait for operation complete or error occur. */
	wcb->exe = 1;

	timeout = 0;
	gettimeofday(&begin, NULL);
	while(wcb->rdy==0 && wcb->err==0) {
		gettimeofday(&now, NULL);
		/* According to Nuvoton's suggestion, few seconds is enough for
	         * longest flash operation, which is erase.
		 * Cutted from W25X16 datasheet, for max operation time
		 *   Byte program        tBP1  50us
		 *   Page program        tPP    3ms
		 *   Sector Erase (4KB)  tSE  200ms
		 *   Block Erase (64KB)  tBE    1s
		 *   Chip Erase          tCE   20s
	         * Since WPCE775x doesn't support chip erase,
		 * 3 secs is long enough for block erase.
		 */
		if ((now.tv_sec - begin.tv_sec) >= 4) {
			timeout += 1;
			break;
		}
	}

	/* keep ERR bit before clearing EXE bit. */
	err = wcb->err;

	/* Clear EXE bit, and wait for RDY back to 0. */
	wcb->exe = 0;
	gettimeofday(&begin, NULL);
	while(wcb->rdy) {
		gettimeofday(&now, NULL);
		/* 1 sec should be long enough for clearing rdy bit. */
		if (((now.tv_sec - begin.tv_sec)*1000*1000 +
	             (now.tv_usec - begin.tv_usec)) >= 1000*1000) {
			timeout += 1;
			break;
		}
	}

	if (err || timeout) {
		msg_cdbg("err=%d timeout=%d\n", err, timeout);
		return 1;
	}
	return 0;
}

/** Initialize the EC parameters.

 *  @return 1 for error; 0 for success.
 */
static int init_flash()
{
	if (!initflash_cfg) {
		msg_perr("%s(): init_flash config is not defined\n", __func__);
		return 1;
	}

	assert_ec_is_free();
	/* Byte 3: command code: Init Flash */
	wcb->code = 0x5A;
	msg_pdbg("%s(): init_flash bytes: ", __func__);
	for (size_t i = 0; i < sizeof(struct wpce775x_initflash_cfg); i++) {
		wcb->field[i] = *((uint8_t *)initflash_cfg + i);
		msg_pdbg("%02x ", wcb->field[i]);
	}
	msg_pdbg("\n");

	if (blocked_exec())
		return 1;
	return 0;
}

/* log2() could be used if we link with -lm */
static int logbase2(int x)
{
	int log = 0;

	/* naive way */
	while (x) {
		x >>= 1;
		log++;
	}
	return log;
}

/* initialize initflash_cfg struct */
static int initflash_cfg_setup(const struct flashctx *flash)
{
	if (!initflash_cfg)
		initflash_cfg = malloc(sizeof(*initflash_cfg));

	/* Set "sane" defaults. If the flash chip is known, then use parameters
	   from it. */
	initflash_cfg->read_device_id = JEDEC_RDID;
	if (flash && (flash->chip->feature_bits | FEATURE_WRSR_WREN))
		initflash_cfg->write_status_enable = JEDEC_WREN;
	else if (flash && (flash->chip->feature_bits | FEATURE_WRSR_EWSR))
		initflash_cfg->write_status_enable = JEDEC_EWSR;
	else
		initflash_cfg->write_status_enable = JEDEC_WREN;
	initflash_cfg->write_enable = JEDEC_WREN;
	initflash_cfg->read_status_register = JEDEC_RDSR;
	initflash_cfg->write_status_register = JEDEC_WRSR;
	initflash_cfg->flash_program = JEDEC_BYTE_PROGRAM;

	/* note: these members are likely to be overridden later */
	initflash_cfg->block_erase = JEDEC_SE;
	initflash_cfg->status_busy_mask = 0x01;
	initflash_cfg->status_reg_value = 0x00;

	/* back to "sane" defaults... */
	initflash_cfg->program_unit_size = 0x01;
	if (flash)
		initflash_cfg->page_size = logbase2(flash->chip->page_size);
	else
		initflash_cfg->page_size = 0x08;

	initflash_cfg->read_device_id_type = 0x00;

	return 0;
}

/** Read flash vendor/device IDs through EC.
 *  @param id0, id1, id2, id3 Pointers to store detected IDs. NULL will be ignored.
 *  @return 1 for error; 0 for success.
 */
static int read_id(const struct flashctx *flash,
	          uint8_t* id0, uint8_t* id1,
	          uint8_t* id2, uint8_t* id3)
{
	if (!initflash_cfg) {
		initflash_cfg_setup(flash);
		init_flash();
	}

	assert_ec_is_free();

	wcb->code = 0xC0;       /* Byte 3: command code: Read ID */
	if (blocked_exec())
		return 1;

	msg_cdbg("id0: 0x%2x, id1: 0x%2x, id2: 0x%2x, id3: 0x%2x\n",
	         wcb->field[0], wcb->field[1], wcb->field[2], wcb->field[3]);
	if (id0) {
		*id0 = wcb->field[0];
	}
	if (id1) {
		*id1 = wcb->field[1];
	}
	if (id2) {
		*id2 = wcb->field[2];
	}
	if (id3) {
		*id3 = wcb->field[3];
	}

	return 0;
}

/** Tell EC to "enter flash update" mode. */
static int enter_flash_update()
{
	if (in_flash_update_mode) {
		/* already in update mode */
		msg_pdbg("%s: in_flash_update_mode: %d\n",
		        __func__, in_flash_update_mode);
		return 0;
	}
	assert_ec_is_free();

	wcb->code = 0x10;  /* Enter Flash Update */
	wcb->field[0] = 0x55;  /* required pattern by EC */
	wcb->field[1] = 0xAA;  /* required pattern by EC */
	wcb->field[2] = 0xCD;  /* required pattern by EC */
	wcb->field[3] = 0xBE;  /* required pattern by EC */
	if (blocked_exec()) {
		return 1;
	} else {
		in_flash_update_mode = 1;
		return 0;
	}
}

/** Tell EC to "exit flash update" mode.
 *  Without calling this function, the EC stays in busy-loop and will not
 *  response further request from host, which means system will halt.
 */
static int exit_flash_update(uint8_t exit_code)
{
	/*
	 * Note: exit_flash_update must be called before shutting down the
	 * machine, otherwise the EC will be stuck in update mode, leaving
	 * the machine in a "wedged" state until power cycled.
	 */
	if (!in_flash_update_mode) {
		msg_cdbg("Not in flash update mode yet.\n");
		return 1;
	}

	wcb->code = exit_code;  /* Exit Flash Update */
	if (blocked_exec()) {
		return 1;
	}

	in_flash_update_mode = 0;
	return 0;
}

/*
 * Note: The EC firmware this patch has been tested with uses the following
 * codes to indicate flash update status:
 * 0x20 is used for EC F/W no change, but BIOS changed (in Share mode)
 * 0x21 is used for EC F/W changed. Goto EC F/W, wait system reboot.
 * 0x22 is used for EC F/W changed, Goto EC Watchdog reset. */
static int exit_flash_update_firmware_no_change(void) {
	return exit_flash_update(0x20);
}

static int exit_flash_update_firmware_changed(void) {
	return exit_flash_update(0x21);
}

static int wpce775x_read(int addr, uint8_t *buf, unsigned int nbytes)
{
        unsigned int bytes_read = 0;

	assert_ec_is_free();
	msg_pspew("%s: reading %d bytes at 0x%06x\n", __func__, nbytes, addr);

	/* Set initial address; WPCE775x auto-increments address for successive
	   read and write operations. */
	wcb->code = 0xA0;
	wcb->field[0] = addr & 0xff;
	wcb->field[1] = (addr >> 8) & 0xff;
	wcb->field[2] = (addr >> 16) & 0xff;
	wcb->field[3] = (addr >> 24) & 0xff;
	if (blocked_exec()) {
		return 1;
	}

	for (unsigned int offset = 0; offset < nbytes; offset += bytes_read) {
		unsigned int bytes_left;

		bytes_left = nbytes - offset;
		if (bytes_left > 0 && bytes_left < WPCE775X_MAX_READ_SIZE)
			bytes_read = bytes_left;
		else
			bytes_read = WPCE775X_MAX_READ_SIZE;
		wcb->code = 0xD0 | bytes_read;
		if (blocked_exec()) {
			return 1;
		}

		for (size_t i = 0; i < bytes_read; i++)
			buf[offset + i] = wcb->field[i];
	}

	return 0;
}

static int wpce775x_erase_new(const struct flashctx *flash, int blockaddr, uint8_t opcode) {
	unsigned int blocksize;
	int ret = 0;

	assert_ec_is_free();

	/*
	 * FIXME: In the long-run we should examine block_erasers within the
	 * flash struct to ensure the proper blocksize is used. This is because
	 * some chips implement commands differently. For now, we'll support
	 * only a few "safe" block erase commands with predictable block size.
	 *
	 * Looking thru the list of flashchips, it seems JEDEC_BE_52 and
	 * JEDEC_BE_D8 are not uniformly implemented. Thus, we cannot safely
	 * assume a blocksize.
	 *
	 * Also, I was unable to test chip erase (due to equipment and time
	 * constraints), but they might work.
	 */
	switch(opcode) {
	case JEDEC_SE:
	case JEDEC_BE_D7:
		blocksize = 4 * 1024;
		break;
	case JEDEC_BE_52:
	case JEDEC_BE_D8:
	case JEDEC_CE_60:
	case JEDEC_CE_C7:
	default:
		msg_perr("%s(): erase opcode=0x%02x not supported\n",
		         __func__, opcode);
		return 1;
	}

	msg_pspew("%s(): blockaddr=%d, blocksize=%d, opcode=0x%02x\n",
	           __func__, blockaddr, blocksize, opcode);

	if (!initflash_cfg)
		initflash_cfg_setup(flash);
	initflash_cfg->block_erase = opcode;
	init_flash();

	/* Set Write Window on flash chip (optional).
	 * You may limit the window to partial flash for experimental. */
	wcb->code = 0xC5;  /* Set Write Window */
	wcb->field[0] = 0x00;  /* window base: little-endian */
	wcb->field[1] = 0x00;
	wcb->field[2] = 0x00;
	wcb->field[3] = 0x00;
	wcb->field[4] = 0x00;  /* window length: little-endian */
	wcb->field[5] = 0x00;
	wcb->field[6] = 0x20;
	wcb->field[7] = 0x00;
	if (blocked_exec())
		return 1;

	msg_pspew("Erasing ... 0x%08x 0x%08x\n", blockaddr, blocksize);

	for (unsigned int current = 0;
	     current < blocksize;
	     current += blocksize) {
		wcb->code = 0x80;  /* Sector/block erase */

		/* WARNING: assume the block address for EC is always little-endian. */
		unsigned int addr = blockaddr + current;
		wcb->field[0] = addr & 0xff;
		wcb->field[1] = (addr >> 8) & 0xff;
		wcb->field[2] = (addr >> 16) & 0xff;
		wcb->field[3] = (addr >> 24) & 0xff;
		if (blocked_exec()) {
			ret = 1;
			goto wpce775x_erase_new_exit;
		}
	}

wpce775x_erase_new_exit:
	firmware_changed = 1;
	return ret;
}

static int wpce775x_nbyte_program(int addr, const uint8_t *buf,
                          unsigned int nbytes)
{
	int ret = 0;
        unsigned int written = 0;

	assert_ec_is_free();
	msg_pspew("%s: writing %d bytes to 0x%06x\n", __func__, nbytes, addr);

	/* Set initial address; WPCE775x auto-increments address for successive
	   read and write operations. */
	wcb->code = 0xA0;
	wcb->field[0] = addr & 0xff;
	wcb->field[1] = (addr >> 8) & 0xff;
	wcb->field[2] = (addr >> 16) & 0xff;
	wcb->field[3] = (addr >> 24) & 0xff;
	if (blocked_exec()) {
		return 1;
	}

	for (unsigned int offset = 0; offset < nbytes; offset += written) {
		unsigned int bytes_left;

		bytes_left = nbytes - offset;
		if (bytes_left > 0 && bytes_left < WPCE775X_MAX_WRITE_SIZE)
			written = bytes_left;
		else
			written = WPCE775X_MAX_WRITE_SIZE;
		wcb->code = 0xB0 | written;

		for (size_t i = 0; i < written; i++)
			wcb->field[i] = buf[offset + i];
		if (blocked_exec()) {
			ret = 1;
			goto wpce775x_nbyte_program_exit;
		}
	}

wpce775x_nbyte_program_exit:
	firmware_changed = 1;
	return ret;
}

static int wpce775x_spi_read(struct flashctx *flash, uint8_t * buf,
                      unsigned int start, unsigned int len)
{
	if (!initflash_cfg) {
		initflash_cfg_setup(flash);
		init_flash();
	}
	return spi_read_chunked(flash, buf, start, len,
				flash->chip->page_size);
}

static int wpce775x_spi_write_256(struct flashctx *flash, const uint8_t *buf,
                           unsigned int start, unsigned int len)
{
	if (!initflash_cfg) {
		initflash_cfg_setup(flash);
		init_flash();
	}
	return spi_write_chunked(flash, buf, start, len,
				 flash->chip->page_size);
}

static int wpce775x_spi_read_status_register(unsigned int readcnt, uint8_t *readarr)
{
	uint8_t before[2];
	int ret = 0;

	assert_ec_is_free();
	msg_pdbg("%s(): reading status register.\n", __func__);

	/* We need to detect if EC firmware support this 0x30 command.
	 * 1. write a non-sense value to field[0] and field[1].
	 * 2. execute command 0x30.
	 * 3. if field[0] is NOT changed, that means 0x30 is not supported,
	 *                use initflash_cfg->status_reg_value instead.
	 *    else, 0x30 works and returns field[].
	 */
	wcb->field[0] = ~initflash_cfg->status_reg_value;
	wcb->field[1] = 0xfc;  /* set reserved bits to 1s */
	/* save original values */
	for (size_t i = 0; i < ARRAY_SIZE(before); i++)
		before[i] = wcb->field[i];

	wcb->code = 0x30;  /* JEDEC_RDSR */
	if (blocked_exec()) {
		ret = 1;
		msg_perr("%s(): blocked_exec() returns error.\n", __func__);
	}
	msg_pdbg("%s(): WCB code=0x%02x field[]= ", __func__, wcb->code);
	for (size_t i = 0; i < readcnt; ++i) {
		readarr[i] = wcb->field[i];
		msg_pdbg("%02x ", wcb->field[i]);
	}
	msg_pdbg("\n");

	/* FIXME: not sure EC returns 1 or 2 bytes for command 0x30,
	 *        now we check field[0] only.
	 *        Shall check field[1] if EC always returns 2 bytes. */
	if (wcb->field[0] == before[0]) {
		/* field is not changed, 0x30 command doesn't work. */
		readarr[0] = initflash_cfg->status_reg_value;
		readarr[1] = 0;  /* TODO: if second status register exists */
		msg_pdbg("%s(): command 0x30 seems NOT working.\n", __func__);
	} else {
		/* 0x30 command seems working! */
		initflash_cfg->status_reg_value = readarr[0];
		msg_pdbg("%s(): command 0x30 seems working.\n", __func__);
	}

	return ret;
}

static int wpce775x_spi_write_status_register(const struct flashctx *flash, uint8_t val)
{
	assert_ec_is_free();
	msg_pdbg("%s(): writing 0x%02x to status register\n", __func__, val);

	if (!initflash_cfg)
		initflash_cfg_setup(flash);

	initflash_cfg->status_reg_value = val;
	if (in_flash_update_mode) {
		exit_flash_update_firmware_no_change();
		in_flash_update_mode = 0;
	}
	if (init_flash())
		return 1;
	if (enter_flash_update())
		return 1;
	return 0;
}

/*
 * WPCE775x does not allow direct access to SPI chip from host. This function
 * will translate SPI commands to valid WPCE775x WCB commands.
 */
static int wpce775x_spi_send_command(const struct flashctx *flash,
				     unsigned int writecnt,
				     unsigned int readcnt,
				     const uint8_t *writearr,
				     uint8_t *readarr)
{
	int rc = 0;
	uint8_t opcode = writearr[0];

	switch(opcode){
	case JEDEC_RDID:{
		uint8_t dummy = 0;
		if (readcnt == 3)
			read_id(flash, &readarr[0], &readarr[1], &readarr[2], &dummy);
		else if (readcnt == 4)
			read_id(flash, &readarr[0], &readarr[1], &readarr[2], &readarr[3]);
		break;
	}
	case JEDEC_RDSR:
		rc = wpce775x_spi_read_status_register(readcnt, readarr);
		break;
	case JEDEC_READ:{
		int blockaddr = ((int)writearr[1] << 16) |
		                ((int)writearr[2] <<  8) |
		                      writearr[3];
		rc = wpce775x_read(blockaddr, readarr, readcnt);
		break;
	}
	case JEDEC_WRSR:
		wpce775x_spi_write_status_register(flash, writearr[1]);
		rc = 0;
		break;
	case JEDEC_WREN:
	case JEDEC_EWSR:
		/* Handled by init_flash() */
		rc = 0;
		break;
	case JEDEC_SE:
	case JEDEC_BE_52:
	case JEDEC_BE_D7:
	case JEDEC_BE_D8:
	case JEDEC_CE_60:
	case JEDEC_CE_C7:{
		int blockaddr = ((int)writearr[1] << 16) |
		                ((int)writearr[2] <<  8) |
		                      writearr[3];

		rc = wpce775x_erase_new(flash, blockaddr, opcode);
		break;
	}
	case JEDEC_BYTE_PROGRAM:{
		int blockaddr = ((int)writearr[1] << 16) |
		                ((int)writearr[2] <<  8) |
		                      writearr[3];
		int nbytes = writecnt - 4;

		rc = wpce775x_nbyte_program(blockaddr, &writearr[4], nbytes);
		break;
	}
	case JEDEC_REMS:
	case JEDEC_RES:
	case JEDEC_WRDI:
	case JEDEC_AAI_WORD_PROGRAM:
	default:
		/* unsupported opcodes */
		msg_pdbg("unsupported SPI opcode: %02x\n", opcode);
		rc = 1;
		break;
	}

	msg_pdbg("%s: opcode: 0x%02x\n", __func__, opcode);
	return rc;
}

static int wpce775x_shutdown(void *data)
{
	msg_pdbg("%s(): firmware %s\n", __func__,
		 firmware_changed ? "changed" : "not changed");

	msg_pdbg("%s: in_flash_update_mode: %d\n", __func__, in_flash_update_mode);
	if (in_flash_update_mode) {
		if (firmware_changed)
			exit_flash_update_firmware_changed();
		else
			exit_flash_update_firmware_no_change();

		in_flash_update_mode = 0;
	}

	if (initflash_cfg)
		free(initflash_cfg);
	else
		msg_perr("%s(): No initflash_cfg to free?!?\n", __func__);

	return 0;
}

static const struct spi_master spi_master_wpce775x = {
	.max_data_read = 256,	/* FIXME: should be MAX_DATA_READ_UNLIMITED? */
	.max_data_write = 256,	/* FIXME: should be MAX_DATA_WRITE_UNLIMITED? */
	.command = wpce775x_spi_send_command,
	.multicommand = default_spi_send_multicommand,
	.read = wpce775x_spi_read,
	.write_256 = wpce775x_spi_write_256,
};

static int wpce775x_spi_common_init(void)
{
	uint8_t fwh_id;

	msg_pdbg("%s(): entered\n", __func__);

	chipset_flash_enable();

	/* get the address of Shadow Window 2. */
	if (get_shaw2ba(&wcb_physical_address) < 0) {
		msg_pdbg("Cannot get the address of Shadow Window 2");
		return 1;
	}
	msg_pdbg("Get the address of WCB(SHA WIN2) at 0x%08lx\n",
	         wcb_physical_address);
	wcb = (struct wpce775x_wcb *)
	      programmer_map_flash_region("WPCE775X WCB",
	                                  wcb_physical_address,
	                                  getpagesize() /* min page size */);
	msg_pdbg("mapped wcb address: %p for physical addr: 0x%08lx\n", wcb, wcb_physical_address);
	if (!wcb) {
		msg_perr("FATAL! Cannot map memory area for wcb physical address.\n");
		return 1;
	}
	memset((void*)wcb, 0, sizeof(*wcb));

	if (get_fwh_id(&fwh_id) < 0) {
		msg_pdbg("Cannot get fwh_id value.\n");
		return 1;
	}
	msg_pdbg("get fwh_id: 0x%02x\n", fwh_id);

	/* TODO: set fwh_idsel of chipset.
	         Currently, we employ "-p internal:fwh_idsel=0x0000223e". */

	if (register_shutdown(wpce775x_shutdown, NULL))
		return 1;

	/* Enter flash update mode unconditionally. This is required even
	   for reading. */
	if (enter_flash_update()) return 1;

	/* Add FWH | LPC to list of buses supported if they are not
	 * both there already. */
	internal_buses_supported |= BUS_FWH | BUS_LPC;
	register_spi_master(&spi_master_wpce775x);
	msg_pdbg("%s(): successfully initialized wpce775x\n", __func__);
	return 0;
}

static int wpce775x_probe_superio()
{
	uint16_t sio_port;
	uint8_t srid;

	/* detect if wpce775x exists */
	if (nuvoton_get_sio_index(&sio_port) < 0) {
		msg_pdbg("No Nuvoton chip is found.\n");
		return 1;
	}
	srid = sio_read(sio_port, NUVOTON_SIOCFG_SRID);
	if ((srid & 0xE0) == 0xA0) {
		msg_pdbg("Found EC: WPCE775x "
			 "(Vendor:0x%02x,ID:0x%02x,Rev:0x%02x) on sio_port:0x%x.\n",
		         sio_read(sio_port, NUVOTON_SIOCFG_SID),
		         srid >> 5, srid & 0x1f, sio_port);
	} else {
		msg_pdbg("Found EC: Nuvoton "
			 "(Vendor:0x%02x,ID:0x%02x,Rev:0x%02x) on sio_port:0x%x.\n",
		         sio_read(sio_port, NUVOTON_SIOCFG_SID),
		         srid >> 5, srid & 0x1f, sio_port);
	}

	return 0;
}

/* Called by internal_init() */
int wpce775x_probe_spi_flash(const char *name)
{
	int ret = 0;
	char *p = NULL;

	p = extract_programmer_param("type");
	if (p && strcmp(p, "ec")) {
		msg_pdbg("mec1308 only supports \"ec\" type devices\n");
		ret = 1;
		goto wpce775x_probe_spi_flash_exit;
	}

	if (wpce775x_probe_superio()) {
		ret = 1;
		goto wpce775x_probe_spi_flash_exit;
	}

	if (wpce775x_spi_common_init()) {
		ret = 1;
		goto wpce775x_probe_spi_flash_exit;
	}

wpce775x_probe_spi_flash_exit:
	free(p);
	return ret;
}
#endif /* __i386__ || __x86_64__ */
