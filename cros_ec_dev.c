/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 Google Inc.
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
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/ioctl.h>
#include <linux/types.h>

#include "file.h"
#include "flash.h"
#include "cros_ec_commands.h"
#include "cros_ec.h"
#include "programmer.h"

#define CROS_EC_DEV_PREFIX "/dev/cros_"
#define CROS_EC_COMMAND_RETRIES	50

int cros_ec_fd;		/* File descriptor for kernel device */

/* The names of the different device that can be found in a machine. */
static const char *ec_type[] = {
	"ec",
	"pd",
	"sh",
	"fp",
	"tp",
};

/*
 * @version: Command version number (often 0)
 * @command: Command to send (EC_CMD_...)
 * @outdata: Outgoing data to EC
 * @outsize: Outgoing length in bytes
 * @indata: Where to put the incoming data from EC
 * @insize: Incoming length in bytes (filled in by EC)
 * @result: EC's response to the command (separate from communication failure)
 */
struct cros_ec_command {
	uint32_t version;
	uint32_t command;
	const uint8_t *outdata;
	uint32_t outsize;
	uint8_t *indata;
	uint32_t insize;
	uint32_t result;
};

#define CROS_EC_DEV_IOC		':'
#define CROS_EC_DEV_IOCXCMD	_IOWR(':', 0, struct cros_ec_command)

/*
 * @version: Command version number (often 0)
 * @command: Command to send (EC_CMD_...)
 * @outsize: Outgoing length in bytes
 * @insize: Max number of bytes to accept from EC
 * @result: EC's response to the command (separate from communication failure)
 * @data: Where to put the incoming data from EC and outgoing data to EC
 */
struct cros_ec_command_v2 {
	uint32_t version;
	uint32_t command;
	uint32_t outsize;
	uint32_t insize;
	uint32_t result;
	uint8_t data[0];
};

#define CROS_EC_DEV_IOC_V2	0xEC
#define CROS_EC_DEV_IOCXCMD_V2	_IOWR(CROS_EC_DEV_IOC_V2, 0, \
				      struct cros_ec_command_v2)

#define CROS_EC_DEV_RETRY	3

/* ec device interface v1 (used with Chrome OS v3.18 and earlier) */

/**
 * Wait for a command to complete, then return the response
 *
 * This is called when we get an EAGAIN response from the EC. We need to
 * send EC_CMD_GET_COMMS_STATUS commands until the EC indicates it is
 * finished the command that we originally sent.
 *
 * returns 0 if command is successful, <0 to indicate timeout or error
 */
static int command_wait_for_response(void)
{
	struct ec_response_get_comms_status status;
	struct cros_ec_command cmd;
	int ret;
	int i;

	cmd.version = 0;
	cmd.command = EC_CMD_GET_COMMS_STATUS;
	cmd.outdata = NULL;
	cmd.outsize = 0;
	cmd.indata = (uint8_t *)&status;
	cmd.insize = sizeof(status);

	/* FIXME: magic delay until we fix the underlying problem (probably in
	   the kernel driver) */
	usleep(10 * 1000);
	for (i = 1; i <= CROS_EC_COMMAND_RETRIES; i++) {
		ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD, &cmd, sizeof(cmd));
		if (ret < 0) {
			msg_perr("%s(): CrOS EC command failed: %d, errno=%d\n",
				 __func__, ret, errno);
			ret = -EC_RES_ERROR;
			break;
		}

		if (cmd.result) {
			msg_perr("%s(): CrOS EC command failed: result=%d\n",
				 __func__, cmd.result);
			ret = -cmd.result;
			break;
		}

		if (!(status.flags & EC_COMMS_STATUS_PROCESSING)) {
			ret = -EC_RES_SUCCESS;
			break;
		}

		usleep(1000);
	}

	return ret;
}

/*
 * __cros_ec_command_dev - Issue command to CROS_EC device
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * This uses the kernel Chrome OS EC driver to communicate with the EC.
 *
 * The outdata and indata buffers contain payload data (if any); command
 * and response codes as well as checksum data are handled transparently by
 * this function.
 *
 * Returns >=0 for success, or negative if other error.
 */
static int __cros_ec_command_dev(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	struct cros_ec_command cmd;
	int ret;

	cmd.version = version;
	cmd.command = command;
	cmd.outdata = outdata;
	cmd.outsize = outsize;
	cmd.indata = indata;
	cmd.insize = insize;
	ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD, &cmd, sizeof(cmd));
	if (ret < 0 && errno == EAGAIN) {
		ret = command_wait_for_response();
		cmd.result = 0;
	}
	if (ret < 0) {
		msg_perr("%s(): Command 0x%02x failed: %d, errno=%d\n",
			__func__, command, ret, errno);
		return -EC_RES_ERROR;
	}
	if (cmd.result) {
		msg_pdbg("%s(): Command 0x%02x returned result: %d\n",
			 __func__, command, cmd.result);
		return -cmd.result;
	}

	return ret;
}

/*
 * ec device interface v2
 * (used with upstream kernel as well as with Chrome OS v4.4 and later)
 */

static int command_wait_for_response_v2(void)
{
	uint8_t s_cmd_buf[sizeof(struct cros_ec_command_v2) +
			  sizeof(struct ec_response_get_comms_status)];
	struct ec_response_get_comms_status *status;
	struct cros_ec_command_v2 *s_cmd;
	int ret;
	int i;

	s_cmd = (struct cros_ec_command_v2 *)s_cmd_buf;
	status = (struct ec_response_get_comms_status *)s_cmd->data;

	s_cmd->version = 0;
	s_cmd->command = EC_CMD_GET_COMMS_STATUS;
	s_cmd->outsize = 0;
	s_cmd->insize = sizeof(*status);

	/*
	 * FIXME: magic delay until we fix the underlying problem (probably in
	 * the kernel driver)
	 */
	usleep(10 * 1000);
	for (i = 1; i <= CROS_EC_COMMAND_RETRIES; i++) {
		ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD_V2, s_cmd_buf,
			    sizeof(s_cmd_buf));
		if (ret < 0) {
			msg_perr("%s(): CrOS EC command failed: %d, errno=%d\n",
				 __func__, ret, errno);
			ret = -EC_RES_ERROR;
			break;
		}
		if (s_cmd->result) {
			msg_perr("%s(): CrOS EC command failed: result=%d\n",
				 __func__, s_cmd->result);
			ret = -s_cmd->result;
			break;
		}

		if (!(status->flags & EC_COMMS_STATUS_PROCESSING)) {
			ret = -EC_RES_SUCCESS;
			break;
		}

		usleep(1000);
	}

	return ret;
}

static int __cros_ec_command_dev_v2(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	struct cros_ec_command_v2 *s_cmd;
	int size = sizeof(struct cros_ec_command_v2) + max(outsize, insize);
	int ret;

	assert(outsize == 0 || outdata != NULL);
	assert(insize == 0 || indata != NULL);

	s_cmd = malloc(size);
	if (s_cmd == NULL)
		return -EC_RES_ERROR;

	s_cmd->command = command;
	s_cmd->version = version;
	s_cmd->result = 0xff;
	s_cmd->outsize = outsize;
	s_cmd->insize = insize;
	memcpy(s_cmd->data, outdata, outsize);

	ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD_V2, s_cmd, size);
	if (ret < 0 && errno == EAGAIN) {
		ret = command_wait_for_response_v2();
		s_cmd->result = 0;
	}
	if (ret < 0) {
		msg_perr("%s(): Command 0x%02x failed: %d, errno=%d\n",
			__func__, command, ret, errno);
		free(s_cmd);
		return -EC_RES_ERROR;
	}
	if (s_cmd->result) {
		msg_pdbg("%s(): Command 0x%02x returned result: %d\n",
			 __func__, command, s_cmd->result);
		free(s_cmd);
		return -s_cmd->result;
	}

	memcpy(indata, s_cmd->data, min(ret, insize));
	free(s_cmd);
	return min(ret, insize);
}

/*
 * Attempt to communicate with kernel using old ioctl format.
 * If it returns ENOTTY, assume that this kernel uses the new format.
 */
static int ec_dev_is_v2()
{
	struct ec_params_hello h_req = {
		.in_data = 0xa0b0c0d0
	};
	struct ec_response_hello h_resp;
	struct cros_ec_command s_cmd = { };
	int r;

	s_cmd.command = EC_CMD_HELLO;
	s_cmd.result = 0xff;
	s_cmd.outsize = sizeof(h_req);
	s_cmd.outdata = (uint8_t *)&h_req;
	s_cmd.insize = sizeof(h_resp);
	s_cmd.indata = (uint8_t *)&h_resp;

	r = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD, &s_cmd, sizeof(s_cmd));
	if (r < 0 && errno == ENOTTY)
		return 1;

	return 0;
}

static int (*__cros_ec_command_dev_fn)(int command, int version,
	const void *outdata, int outsize, void *indata, int insize);

/*
 * cros_ec_command_dev - Issue command to CROS_EC device with retry
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * This uses the kernel Chrome OS EC driver to communicate with the EC.
 *
 * The outdata and indata buffers contain payload data (if any); command
 * and response codes as well as checksum data are handled transparently by
 * this function.
 *
 * Returns >=0 for success, or negative if other error.
 */
static int cros_ec_command_dev(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	int ret = EC_RES_ERROR;
	int try;

	for (try = 0; try < CROS_EC_DEV_RETRY; try++) {
		ret = __cros_ec_command_dev_fn(command, version, outdata,
					       outsize, indata, insize);
		if (ret >= 0)
			return ret;
	}

	return ret;
}

static void cros_ec_set_max_size(struct cros_ec_priv *priv, struct opaque_master *op)
{
	struct ec_response_get_protocol_info info;
	int rc = 0;

	msg_pdbg("%s: sending protoinfo command\n", __func__);
	rc = priv->ec_command(EC_CMD_GET_PROTOCOL_INFO, 0, NULL, 0,
			      &info, sizeof(info));
	msg_pdbg("%s: rc:%d\n", __func__, rc);

	/*
	 * Use V3 large size only if v2 protocol is not supported.
	 * When v2 is supported, we may be using a kernel without v3 support,
	 * leading to sending larger commands the kernel can support.
	 */
	if (rc == sizeof(info) && ((info.protocol_versions & (1<<2)) == 0)) {
		/* Allow overriding the max response size in case EC is incorrect */
		if (priv->max_response_size)
			info.max_response_packet_size = priv->max_response_size;

		op->max_data_write = info.max_request_packet_size -
			sizeof(struct ec_host_request);
		op->max_data_read = info.max_response_packet_size -
			sizeof(struct ec_host_response);
		/*
		 * Due to a bug in NPCX SPI code (chromium:725580),
		 * The EC may responds 163 when it meant 160; it should not
		 * have included header and footer.
		 */
		op->max_data_read &= ~3;
		msg_pdbg("%s: max_write:%d max_read:%d\n", __func__,
			 op->max_data_write, op->max_data_read);
	}
}

/*
 * Returns 0 to indicate success, non-zero otherwise
 *
 * This function parses programmer parameters from the command line. Since
 * CrOS EC hangs off the "internal programmer" (AP, PCH, etc) this gets
 * run during internal programmer initialization.
 */
static int cros_ec_parse_param(struct cros_ec_priv *priv)
{
	char *p;

	p = extract_programmer_param("type");
	if (p) {
		unsigned int index;
		for (index = 0; index < ARRAY_SIZE(ec_type); index++)
			if (!strcmp(p, ec_type[index]))
				break;
		if (index == ARRAY_SIZE(ec_type)) {
			msg_perr("Invalid argument: \"%s\"\n", p);
			free(p);
			return 1;
		}
		priv->dev = ec_type[index];
		msg_pdbg("Target %s used\n", priv->dev);
	}
	free(p);

	p = extract_programmer_param("block");
	if (p) {
		unsigned int block;
		char *endptr = NULL;

		errno = 0;
		block = strtoul(p, &endptr, 0);
		if (errno || (strlen(p) > 10) || (endptr != (p + strlen(p)))) {
			msg_perr("Invalid argument: \"%s\"\n", p);
			free(p);
			return 1;
		}

		if (block <= 0) {
			msg_perr("%s: Invalid block size\n", __func__);
			free(p);
			return 1;
		}

		msg_pdbg("Override block size to 0x%x\n", block);
		priv->erase_block_size = block;
	}
	free(p);

	p = extract_programmer_param("max_response_size");
	if (p) {
		unsigned int max_response_size;
		char *endptr = NULL;

		errno = 0;
		max_response_size = strtoul(p, &endptr, 0);
		if (errno || (strlen(p) > 10) || (endptr != (p + strlen(p)))) {
			msg_perr("Invalid max_response_size: \"%s\"\n", p);
			free(p);
			return 1;
		}

		if (max_response_size <= 0) {
			msg_perr("%s: Invalid max_response_size\n", __func__);
			free(p);
			return 1;
		}

		msg_pdbg("Override max response size to 0x%x\n", max_response_size);
		priv->max_response_size = max_response_size;
	}
	free(p);

	return 0;
}

/* perform basic "hello" test to see if we can talk to the EC */
static int cros_ec_test(struct cros_ec_priv *priv)
{
	struct ec_params_hello request;
	struct ec_response_hello response;
	int rc = 0;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	msg_pdbg("%s: sending HELLO request with 0x%08x\n",
	         __func__, request.in_data);
	rc = priv->ec_command(EC_CMD_HELLO, 0, &request,
			     sizeof(request), &response, sizeof(response));
	msg_pdbg("%s: response: 0x%08x\n", __func__, response.out_data);

	if (rc < 0 || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
		return 1;
	}

	return 0;
}


static struct cros_ec_priv cros_ec_dev_priv = {
	.detected	= 0,
	.ec_command	= cros_ec_command_dev,
	.dev = "ec",
};

static struct opaque_master opaque_master_cros_ec_dev = {
	.max_data_read	= 128,
	.max_data_write	= 128,
	.probe		= cros_ec_probe_size,
	.read		= cros_ec_read,
	.write		= cros_ec_write,
	.erase		= cros_ec_block_erase,
};

static int cros_ec_dev_shutdown(void *data)
{
	close(cros_ec_fd);
	return 0;
}

int cros_ec_probe_dev(void)
{
	char dev_path[32];

	if (alias && alias->type != ALIAS_EC)
		return 1;

	if (cros_ec_parse_param(&cros_ec_dev_priv))
		return 1;

	snprintf(dev_path, sizeof(dev_path), "%s%s",
			CROS_EC_DEV_PREFIX, cros_ec_dev_priv.dev);

	msg_pdbg("%s: probing for CROS_EC at %s\n", __func__, dev_path);
	cros_ec_fd = open(dev_path, O_RDWR);
	if (cros_ec_fd < 0)
		return cros_ec_fd;

	if (ec_dev_is_v2())
		__cros_ec_command_dev_fn = __cros_ec_command_dev_v2;
	else
		__cros_ec_command_dev_fn = __cros_ec_command_dev;

	if (cros_ec_test(&cros_ec_dev_priv))
		return 1;

	cros_ec_set_max_size(&cros_ec_dev_priv, &opaque_master_cros_ec_dev);

	msg_pdbg("CROS_EC detected at %s\n", dev_path);
	register_opaque_master(&opaque_master_cros_ec_dev);
	register_shutdown(cros_ec_dev_shutdown, NULL);
	cros_ec_dev_priv.detected = 1;
	cros_ec_priv = &cros_ec_dev_priv;

	return 0;
}
