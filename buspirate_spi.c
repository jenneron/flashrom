/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009, 2010 Carl-Daniel Hailfinger
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

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "flash.h"
#include "programmer.h"
#include "spi.h"

/* Change this to #define if you want to test without a serial implementation */
#undef FAKE_COMMUNICATION

struct buspirate_spispeeds {
	const char *name;
	const int speed;
};

#ifndef FAKE_COMMUNICATION
static int buspirate_serialport_setup(char *dev)
{
	/* 115200bps, 8 databits, no parity, 1 stopbit */
	sp_fd = sp_openserport(dev, 115200);
	return 0;
}
#else
#define buspirate_serialport_setup(...) 0
#define serialport_shutdown(...) 0
#define serialport_write(...) 0
#define serialport_read(...) 0
#define sp_flush_incoming(...) 0
#endif

static unsigned char *bp_commbuf = NULL;
static int bp_commbufsize = 0;

static int buspirate_commbuf_grow(int bufsize)
{
	unsigned char *tmpbuf;

	/* Never shrink. realloc() calls are expensive. */
	if (bufsize <= bp_commbufsize)
		return 0;

	tmpbuf = realloc(bp_commbuf, bufsize);
	if (!tmpbuf) {
		/* Keep the existing buffer because memory is already tight. */
		msg_perr("Out of memory!\n");
		return ERROR_OOM;
	}

	bp_commbuf = tmpbuf;
	bp_commbufsize = bufsize;
	return 0;
}

static int buspirate_sendrecv(unsigned char *buf, unsigned int writecnt,
			      unsigned int readcnt)
{
	int i, ret = 0;

	msg_pspew("%s: write %i, read %i ", __func__, writecnt, readcnt);
	if (!writecnt && !readcnt) {
		msg_perr("Zero length command!\n");
		return 1;
	}
	msg_pspew("Sending");
	for (i = 0; i < writecnt; i++)
		msg_pspew(" 0x%02x", buf[i]);
#ifdef FAKE_COMMUNICATION
	/* Placate the caller for now. */
	if (readcnt) {
		buf[0] = 0x01;
		memset(buf + 1, 0xff, readcnt - 1);
	}
	ret = 0;
#else
	if (writecnt)
		ret = serialport_write(buf, writecnt);
	if (ret)
		return ret;
	if (readcnt)
		ret = serialport_read(buf, readcnt);
	if (ret)
		return ret;
#endif
	msg_pspew(", receiving");
	for (i = 0; i < readcnt; i++)
		msg_pspew(" 0x%02x", buf[i]);
	msg_pspew("\n");
	return 0;
}

static int buspirate_spi_send_command(const struct flashctx *flash,
				      unsigned int writecnt,
				      unsigned int readcnt,
				      const unsigned char *writearr,
				      unsigned char *readarr);

static const struct spi_master spi_master_buspirate = {
	.type		= SPI_CONTROLLER_BUSPIRATE,
	.features	= SPI_MASTER_4BA,
	.max_data_read	= 12,
	.max_data_write	= 12,
	.command	= buspirate_spi_send_command,
	.multicommand	= default_spi_send_multicommand,
	.read		= default_spi_read,
	.write_256	= default_spi_write_256,
};

static const struct buspirate_spispeeds spispeeds[] = {
	{"30k",		0x0},
	{"125k",	0x1},
	{"250k",	0x2},
	{"1M",		0x3},
	{"2M",		0x4},
	{"2.6M",	0x5},
	{"4M",		0x6},
	{"8M",		0x7},
	{NULL,		0x0},
};

static int buspirate_spi_shutdown(void *data)
{
	int ret = 0, ret2 = 0;
	/* No need to allocate a buffer here, we know that bp_commbuf is at least DEFAULT_BUFSIZE big. */

	/* Exit raw SPI mode (enter raw bitbang mode) */
	bp_commbuf[0] = 0x00;
	ret = buspirate_sendrecv(bp_commbuf, 1, 5);
	if (ret)
		goto out_shutdown;
	if (memcmp(bp_commbuf, "BBIO", 4)) {
		msg_perr("Entering raw bitbang mode failed!\n");
		ret = 1;
		goto out_shutdown;
	}
	msg_pdbg("Raw bitbang mode version %c\n", bp_commbuf[4]);
	if (bp_commbuf[4] != '1') {
		msg_perr("Can't handle raw bitbang mode version %c!\n", bp_commbuf[4]);
		ret = 1;
		goto out_shutdown;
	}
	/* Reset Bus Pirate (return to user terminal) */
	bp_commbuf[0] = 0x0f;
	ret = buspirate_sendrecv(bp_commbuf, 1, 0);

out_shutdown:
	/* Shut down serial port communication */
	ret2 = serialport_shutdown(NULL);
	/* Keep the oldest error, it is probably the best indicator. */
	if (ret2 && !ret)
		ret = ret2;
	bp_commbufsize = 0;
	free(bp_commbuf);
	bp_commbuf = NULL;
	if (ret)
		msg_pdbg("Bus Pirate shutdown failed.\n");
	else
		msg_pdbg("Bus Pirate shutdown completed.\n");

	return ret;
}

int buspirate_spi_init(void)
{
	char *dev = NULL;
	char *speed = NULL;
	int spispeed = 0x7;
	int ret = 0;
	int i;

	dev = extract_programmer_param("dev");
	if (!dev || !strlen(dev)) {
		msg_perr("No serial device given. Use flashrom -p "
			"buspirate_spi:dev=/dev/ttyUSB0\n");
		return 1;
	}

	speed = extract_programmer_param("spispeed");
	if (speed) {
		for (i = 0; spispeeds[i].name; i++)
			if (!strncasecmp(spispeeds[i].name, speed,
			    strlen(spispeeds[i].name))) {
				spispeed = spispeeds[i].speed;
				break;
			}
		if (!spispeeds[i].name)
			msg_perr("Invalid SPI speed, using default.\n");
	}
	free(speed);

	/* This works because speeds numbering starts at 0 and is contiguous. */
	msg_pdbg("SPI speed is %sHz\n", spispeeds[spispeed].name);

	/* Default buffer size is 19: 16 bytes data, 3 bytes control. */
#define DEFAULT_BUFSIZE (16 + 3)
	bp_commbuf = malloc(DEFAULT_BUFSIZE);
	if (!bp_commbuf) {
		bp_commbufsize = 0;
		msg_perr("Out of memory!\n");
		return ERROR_OOM;
	}
	bp_commbufsize = DEFAULT_BUFSIZE;

	ret = buspirate_serialport_setup(dev);
	free(dev);
	if (ret) {
		bp_commbufsize = 0;
		free(bp_commbuf);
		bp_commbuf = NULL;
		return ret;
	}

	if (register_shutdown(buspirate_spi_shutdown, NULL))
		return 1;

	/* This is the brute force version, but it should work. */
	for (i = 0; i < 19; i++) {
		/* Enter raw bitbang mode */
		bp_commbuf[0] = 0x00;
		/* Send the command, don't read the response. */
		ret = buspirate_sendrecv(bp_commbuf, 1, 0);
		if (ret)
			return ret;
		/* Read any response and discard it. */
		sp_flush_incoming();
	}
	/* USB is slow. The Bus Pirate is even slower. Apparently the flush
	 * action above is too fast or too early. Some stuff still remains in
	 * the pipe after the flush above, and one additional flush is not
	 * sufficient either. Use a 1.5 ms delay inside the loop to make
	 * mostly sure that at least one USB frame had time to arrive.
	 * Looping only 5 times is not sufficient and causes the
	 * occasional failure.
	 * Folding the delay into the loop above is not reliable either.
	 */
	for (i = 0; i < 10; i++) {
		usleep(1500);
		/* Read any response and discard it. */
		sp_flush_incoming();
	}
	/* Enter raw bitbang mode */
	bp_commbuf[0] = 0x00;
	ret = buspirate_sendrecv(bp_commbuf, 1, 5);
	if (ret)
		return ret;
	if (memcmp(bp_commbuf, "BBIO", 4)) {
		msg_perr("Entering raw bitbang mode failed!\n");
		msg_pdbg("Got %02x%02x%02x%02x%02x\n",
			 bp_commbuf[0], bp_commbuf[1], bp_commbuf[2],
			 bp_commbuf[3], bp_commbuf[4]);
		return 1;
	}
	msg_pdbg("Raw bitbang mode version %c\n", bp_commbuf[4]);
	if (bp_commbuf[4] != '1') {
		msg_perr("Can't handle raw bitbang mode version %c!\n",
			bp_commbuf[4]);
		return 1;
	}
	/* Enter raw SPI mode */
	bp_commbuf[0] = 0x01;
	ret = buspirate_sendrecv(bp_commbuf, 1, 4);
	if (ret)
		return ret;
	if (memcmp(bp_commbuf, "SPI", 3)) {
		msg_perr("Entering raw SPI mode failed!\n");
		msg_pdbg("Got %02x%02x%02x%02x\n",
			 bp_commbuf[0], bp_commbuf[1], bp_commbuf[2],
			 bp_commbuf[3]);
		return 1;
	}
	msg_pdbg("Raw SPI mode version %c\n", bp_commbuf[3]);
	if (bp_commbuf[3] != '1') {
		msg_perr("Can't handle raw SPI mode version %c!\n",
			bp_commbuf[3]);
		return 1;
	}

	/* Initial setup (SPI peripherals config): Enable power, CS high, AUX */
	bp_commbuf[0] = 0x40 | 0xb;
	ret = buspirate_sendrecv(bp_commbuf, 1, 1);
	if (ret)
		return 1;
	if (bp_commbuf[0] != 0x01) {
		msg_perr("Protocol error while setting power/CS/AUX!\n");
		return 1;
	}

	/* Set SPI speed */
	bp_commbuf[0] = 0x60 | spispeed;
	ret = buspirate_sendrecv(bp_commbuf, 1, 1);
	if (ret)
		return 1;
	if (bp_commbuf[0] != 0x01) {
		msg_perr("Protocol error while setting SPI speed!\n");
		return 1;
	}
	
	/* Set SPI config: output type, idle, clock edge, sample */
	bp_commbuf[0] = 0x80 | 0xa;
	ret = buspirate_sendrecv(bp_commbuf, 1, 1);
	if (ret)
		return 1;
	if (bp_commbuf[0] != 0x01) {
		msg_perr("Protocol error while setting SPI config!\n");
		return 1;
	}

	/* De-assert CS# */
	bp_commbuf[0] = 0x03;
	ret = buspirate_sendrecv(bp_commbuf, 1, 1);
	if (ret)
		return 1;
	if (bp_commbuf[0] != 0x01) {
		msg_perr("Protocol error while raising CS#!\n");
		return 1;
	}

	register_spi_master(&spi_master_buspirate);

	return 0;
}

static int buspirate_spi_send_command(const struct flashctx *flash,
				      unsigned int writecnt,
				      unsigned int readcnt,
				      const unsigned char *writearr,
				      unsigned char *readarr)
{
	unsigned int i = 0;
	int ret = 0;

	if (writecnt > 16 || readcnt > 16 || (readcnt + writecnt) > 16)
		return SPI_INVALID_LENGTH;

	/* 3 bytes extra for CS#, len, CS#. */
	if (buspirate_commbuf_grow(writecnt + readcnt + 3))
		return ERROR_OOM;

	/* Assert CS# */
	bp_commbuf[i++] = 0x02;

	bp_commbuf[i++] = 0x10 | (writecnt + readcnt - 1);
	memcpy(bp_commbuf + i, writearr, writecnt);
	i += writecnt;
	memset(bp_commbuf + i, 0, readcnt);

	i += readcnt;
	/* De-assert CS# */
	bp_commbuf[i++] = 0x03;

	ret = buspirate_sendrecv(bp_commbuf, i, i);

	if (ret) {
		msg_perr("Bus Pirate communication error!\n");
		return SPI_GENERIC_ERROR;
	}

	if (bp_commbuf[0] != 0x01) {
		msg_perr("Protocol error while lowering CS#!\n");
		return SPI_GENERIC_ERROR;
	}

	if (bp_commbuf[1] != 0x01) {
		msg_perr("Protocol error while reading/writing SPI!\n");
		return SPI_GENERIC_ERROR;
	}

	if (bp_commbuf[i - 1] != 0x01) {
		msg_perr("Protocol error while raising CS#!\n");
		return SPI_GENERIC_ERROR;
	}

	/* Skip CS#, length, writearr. */
	memcpy(readarr, bp_commbuf + 2 + writecnt, readcnt);

	return ret;
}
