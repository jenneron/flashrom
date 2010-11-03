/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2007, 2008, 2009 Carl-Daniel Hailfinger
 * Copyright (C) 2008 Ronald Hoogenboom <ronald@zonnet.nl>
 * Copyright (C) 2008 coresystems GmbH
 * Copyright (C) 2010 Google Inc.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
 * Contains the ITE IT85* SPI specific routines
 */

#if defined(__i386__) || defined(__x86_64__)

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "flash.h"
#include "chipdrivers.h"
#include "spi.h"
#include "programmer.h"

#define MAX_TIMEOUT 100000
#define MAX_TRY 5

/* Constans for I/O ports */
#define ITE_SUPERIO_PORT1	0x2e
#define ITE_SUPERIO_PORT2	0x4e

/* Legacy I/O */
#define LEGACY_KBC_PORT_DATA	0x60
#define LEGACY_KBC_PORT_CMD	0x64

/* Constants for Logical Device registers */
#define LDNSEL			0x07
#define CHIP_ID_BYTE1_REG	0x20
#define CHIP_ID_BYTE2_REG	0x21
#define CHIP_CHIP_VER_REG	0x22

/* These are standard Super I/O 16-bit base address registers */
#define SHM_IO_BAD0		0x60  /* big-endian, this is high bits */
#define SHM_IO_BAD1		0x61

/* 8042 keyboard controller uses an input buffer and an output buffer to
 * communicate with host CPU. Both buffers are 1-byte depth. That means the
 * IBF is set to 1 when host CPU sends a command to input buffer (standing on
 * the EC side). IBF is cleared to 0 once the command is read by EC. */
#define KB_IBF 			(1 << 1)  /* Input Buffer Full */
#define KB_OBF 			(1 << 0)  /* Output Buffer Full */

/* IT8502 supports two access modes:
 *   LPC_MEMORY: through the memory window in 0xFFFFFxxx (follow mode)
 *   LPC_IO: through I/O port (so called indirect memory)
 */
#undef LPC_MEMORY
#define LPC_IO

#ifdef LPC_IO
/* macro to fill in indirect-access registers. */
#define INDIRECT_A0(base, value) OUTB(value, (base) + 0)  /* little-endian */
#define INDIRECT_A1(base, value) OUTB(value, (base) + 1)
#define INDIRECT_A2(base, value) OUTB(value, (base) + 2)
#define INDIRECT_A3(base, value) OUTB(value, (base) + 3)
#define INDIRECT_READ(base) INB((base) + 4)
#define INDIRECT_WRITE(base, value) OUTB(value, (base) + 4)
#endif  /* LPC_IO */

#ifdef LPC_IO
unsigned int shm_io_base;
#endif
unsigned char *ce_high, *ce_low;
static int it85xx_scratch_rom_reenter = 0;

uint16_t probe_id_ite85(uint16_t port)
{
	uint16_t id;

	id = sio_read(port, CHIP_ID_BYTE1_REG) << 8 |
	     sio_read(port, CHIP_ID_BYTE2_REG);

	return id;
}

struct superio probe_superio_ite85xx(void)
{
	struct superio ret = {};
	uint16_t ite_ports[] = {ITE_SUPERIO_PORT1, ITE_SUPERIO_PORT2, 0};
	uint16_t *i = ite_ports;

	ret.vendor = SUPERIO_VENDOR_ITE;
	for (; *i; i++) {
		ret.port = *i;
		ret.model = probe_id_ite85(ret.port);
		switch (ret.model >> 8) {
		case 0x85:
			msg_pdbg("Found EC: ITE85xx (Vendor:0x%02x,ID:0x%02x,"
			         "Rev:0x%02x) on sio_port:0x%x.\n",
			         ret.model >> 8, ret.model & 0xff,
			         sio_read(ret.port, CHIP_CHIP_VER_REG),
			         ret.port);
			return ret;
		}
	}

	/* No good ID found. */
	ret.vendor = SUPERIO_VENDOR_NONE;
	ret.port = 0;
	ret.model = 0;
	return ret;
}

/* This function will poll the keyboard status register until either
 *   an expected value shows up, or
 *   timeout reaches.
 *
 * Returns: 0 -- the expected value has shown.
 *          1 -- timeout reached.
 */
static int wait_for(
		const unsigned int mask,
		const unsigned int expected_value,
		const int timeout,  /* in usec */
		const char* error_message,
		const char* function_name,
		const int lineno
) {
	int time_passed;

	for (time_passed = 0;; ++time_passed) {
		if ((INB(LEGACY_KBC_PORT_CMD) & mask) == expected_value)
			return 0;
		if (time_passed >= timeout)
			break;
		programmer_delay(1);
	}
	if (error_message)
		msg_perr("%s():%d %s", function_name, lineno, error_message);
	return 1;
}

/* IT8502 employs a scratch ram when flash is being updated. Call the following
 * two functions before/after flash erase/program. */
void it85xx_enter_scratch_rom()
{
	int ret;
	int tries;

	msg_pdbg("%s():%d was called ...\n", __FUNCTION__, __LINE__);
	if (it85xx_scratch_rom_reenter > 0) return;

	/* FIXME: this a workaround for the bug that SMBus signal would
	 *        interfere the EC firmware update. Should be removed if
	 *        we find out the root cause. */
	ret = system("stop powerd >&2");
	if (ret) {
		msg_perr("Cannot stop powerd.\n");
	}

	for (tries = 0; tries < MAX_TRY; ++tries) {
		/* Wait until IBF (input buffer) is not full. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at waiting for IBF==0.\n",
		             __FUNCTION__, __LINE__))
			continue;

		/* Copy EC firmware to SRAM. */
		OUTB(0xb4, LEGACY_KBC_PORT_CMD);

		/* Confirm EC has taken away the command. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at taking command.\n",
		             __FUNCTION__, __LINE__))
			continue;

		/* Waiting for OBF (output buffer) has data.
		 * Note sometimes the replied command might be stolen by kernel
		 * ISR so that it is okay as long as the command is 0xFA. */
		if (wait_for(KB_OBF, KB_OBF, MAX_TIMEOUT, NULL, NULL, 0))
			msg_pdbg("%s():%d * timeout at waiting for OBF.\n",
			         __FUNCTION__, __LINE__);
		if ((ret = INB(LEGACY_KBC_PORT_DATA)) == 0xFA) {
			break;
		} else {
			msg_perr("%s():%d * not run on SRAM ret=%d\n",
			         __FUNCTION__, __LINE__, ret);
			continue;
		}
	}

	if (tries < MAX_TRY) {
		/* EC already runs on SRAM */
		it85xx_scratch_rom_reenter++;
		msg_pdbg("%s():%d * SUCCESS.\n", __FUNCTION__, __LINE__);
	} else {
		msg_perr("%s():%d * Max try reached.\n",
		         __FUNCTION__, __LINE__);
	}
}

void it85xx_exit_scratch_rom()
{
	int ret;
	int tries;

	msg_pdbg("%s():%d was called ...\n", __FUNCTION__, __LINE__);
	if (it85xx_scratch_rom_reenter <= 0) return;

	for (tries = 0; tries < MAX_TRY; ++tries) {
		/* Wait until IBF (input buffer) is not full. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at waiting for IBF==0.\n",
		             __FUNCTION__, __LINE__))
			continue;

		/* Exit SRAM. Run on flash. */
		OUTB(0xFE, LEGACY_KBC_PORT_CMD);

		/* Confirm EC has taken away the command. */
		if (wait_for(KB_IBF, 0, MAX_TIMEOUT,
		             "* timeout at taking command.\n",
		             __FUNCTION__, __LINE__)) {
			/* We cannot ensure if EC has exited update mode.
			 * If EC is in normal mode already, a further 0xFE
			 * command will reboot system. So, exit loop here. */
			tries = MAX_TRY;
			break;
		}

		break;
	}

	if (tries < MAX_TRY) {
		it85xx_scratch_rom_reenter = 0;
		msg_pdbg("%s():%d * SUCCESS.\n", __FUNCTION__, __LINE__);
	} else {
		msg_perr("%s():%d * Max try reached.\n",
		         __FUNCTION__, __LINE__);
	}

	/* FIXME: this a workaround for the bug that SMBus signal would
	 *        interfere the EC firmware update. Should be removed if
	 *        we find out the root cause. */
	ret = system("start powerd >&2");
	if (ret) {
		msg_perr("Cannot start powerd again.\n");
	}
}

int it85xx_spi_common_init(void)
{
	chipaddr base;

	msg_pdbg("%s():%d superio.vendor=0x%02x\n", __func__, __LINE__,
	         superio.vendor);
	if (superio.vendor != SUPERIO_VENDOR_ITE)
		return 1;

#ifdef LPC_IO
	/* Get LPCPNP of SHM. That's big-endian */
	sio_write(superio.port, LDNSEL, 0x0F); /* Set LDN to SHM (0x0F) */
	shm_io_base = (sio_read(superio.port, SHM_IO_BAD0) << 8) +
	              sio_read(superio.port, SHM_IO_BAD1);
	msg_pdbg("%s():%d shm_io_base=0x%04x\n", __func__, __LINE__,
	         shm_io_base);

	/* These pointers are not used directly. They will be send to EC's
	 * register for indirect access. */
	base = 0xFFFFF000;
	ce_high = ((unsigned char*)base) + 0xE00;  /* 0xFFFFFE00 */
	ce_low = ((unsigned char*)base) + 0xD00;  /* 0xFFFFFD00 */

	/* pre-set indirect-access registers since in most of cases they are
	 * 0xFFFFxx00. */
	INDIRECT_A0(shm_io_base, base & 0xFF);
	INDIRECT_A2(shm_io_base, (base >> 16) & 0xFF);
	INDIRECT_A3(shm_io_base, (base >> 24));
#endif
#ifdef LPC_MEMORY
	base = (chipaddr)programmer_map_flash_region("flash base", 0xFFFFF000,
	                                             0x1000);
	msg_pdbg("%s():%d base=0x%08x\n", __func__, __LINE__,
	         (unsigned int)base);
	ce_high = (unsigned char*)(base + 0xE00);  /* 0xFFFFFE00 */
	ce_low = (unsigned char*)(base + 0xD00);  /* 0xFFFFFD00 */
#endif

	/* Set this as spi controller. */
	spi_controller = SPI_CONTROLLER_IT85XX;

	return 0;
}

/* Called by programmer_entry .init */
int it85xx_spi_init(void)
{
	int ret;

	get_io_perms();
	/* Probe for the Super I/O chip and fill global struct superio. */
	probe_superio();
	ret = it85xx_spi_common_init();
	if (!ret) {
		buses_supported = CHIP_BUSTYPE_SPI;
	} else {
		buses_supported = CHIP_BUSTYPE_NONE;
	}
	return ret;
}

/* Called by internal_init() */
int it85xx_probe_spi_flash(const char *name)
{
	int ret;

	if (!(buses_supported & CHIP_BUSTYPE_FWH)) {
		msg_pdbg("%s():%d buses not support FWH\n", __func__, __LINE__);
		return 1;
	}
	ret = it85xx_spi_common_init();
	msg_pdbg("FWH: %s():%d ret=%d\n", __func__, __LINE__, ret);
	if (!ret) {
		msg_pdbg("%s():%d buses_supported=0x%x\n", __func__, __LINE__,
		          buses_supported);
		if (buses_supported & CHIP_BUSTYPE_FWH)
			msg_pdbg("Overriding chipset SPI with IT85 FWH|SPI.\n");
		buses_supported |= CHIP_BUSTYPE_FWH | CHIP_BUSTYPE_SPI;
	}
	return ret;
}

int it85xx_shutdown(void)
{
	msg_pdbg("%s():%d\n", __func__, __LINE__);
	it85xx_exit_scratch_rom();
	return 0;
}

/* According to ITE 8502 document, the procedure to follow mode is following:
 *   1. write 0x00 to LPC/FWH address 0xffff_fexxh (drive CE# high)
 *   2. write data to LPC/FWH address 0xffff_fdxxh (drive CE# low and MOSI
 *      with data)
 *   3. read date from LPC/FWH address 0xffff_fdxxh (drive CE# low and get
 *      data from MISO)
 */
int it85xx_spi_send_command(unsigned int writecnt, unsigned int readcnt,
			const unsigned char *writearr, unsigned char *readarr)
{
	int i;

	it85xx_enter_scratch_rom();
	/* exit scratch rom ONLY when programmer shuts down. Otherwise, the
	 * temporary flash state may halt EC. */

#ifdef LPC_IO
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_high) >> 8) & 0xff);
	INDIRECT_WRITE(shm_io_base, 0xFF);  /* Write anything to this address.*/
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_low) >> 8) & 0xff);
#endif
#ifdef LPC_MEMORY
	*ce_high = 0;
#endif
	for (i = 0; i < writecnt; ++i) {
#ifdef LPC_IO
		INDIRECT_WRITE(shm_io_base, writearr[i]);
#endif
#ifdef LPC_MEMORY
		*ce_low = writearr[i];
#endif
	}
	for (i = 0; i < readcnt; ++i) {
#ifdef LPC_IO
		readarr[i] = INDIRECT_READ(shm_io_base);
#endif
#ifdef LPC_MEMORY
		readarr[i] = *ce_low;
#endif
	}
#ifdef LPC_IO
	INDIRECT_A1(shm_io_base, (((unsigned long int)ce_high) >> 8) & 0xff);
	INDIRECT_WRITE(shm_io_base, 0xFF);  /* Write anything to this address.*/
#endif
#ifdef LPC_MEMORY
	*ce_high = 0;
#endif

	return 0;
}

#endif
