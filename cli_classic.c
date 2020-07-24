/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2004 Tyan Corp <yhlu@tyan.com>
 * Copyright (C) 2005-2008 coresystems GmbH
 * Copyright (C) 2008,2009,2010 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include "big_lock.h"
#include "flash.h"
#include "flashchips.h"
#include "layout.h"
#include "power.h"
#include "programmer.h"
#include "writeprotect.h"

#define LOCK_TIMEOUT_SECS	180

int set_ignore_lock = 0;

#if CONFIG_INTERNAL == 1
static enum programmer default_programmer = PROGRAMMER_INTERNAL;
#elif CONFIG_DUMMY == 1
static enum programmer default_programmer = PROGRAMMER_DUMMY;
#else
/* If neither internal nor dummy are selected, we must pick a sensible default.
 * Since there is no reason to prefer a particular external programmer, we fail
 * if more than one of them is selected. If only one is selected, it is clear
 * that the user wants that one to become the default.
 */
#if CONFIG_NIC3COM+CONFIG_NICREALTEK+CONFIG_NICNATSEMI+CONFIG_GFXNVIDIA+CONFIG_DRKAISER+CONFIG_SATASII+CONFIG_ATAHPT+CONFIG_FT2232_SPI+CONFIG_SERPROG+CONFIG_BUSPIRATE_SPI+CONFIG_DEDIPROG+CONFIG_RAYER_SPI+CONFIG_NICINTEL+CONFIG_NICINTEL_SPI+CONFIG_OGP_SPI+CONFIG_SATAMV > 1
#error Please enable either CONFIG_DUMMY or CONFIG_INTERNAL or disable support for all programmers except one.
#endif
static enum programmer default_programmer =
#if CONFIG_NIC3COM == 1
	PROGRAMMER_NIC3COM
#endif
#if CONFIG_NICREALTEK == 1
	PROGRAMMER_NICREALTEK
#endif
#if CONFIG_NICNATSEMI == 1
	PROGRAMMER_NICNATSEMI
#endif
#if CONFIG_GFXNVIDIA == 1
	PROGRAMMER_GFXNVIDIA
#endif
#if CONFIG_DRKAISER == 1
	PROGRAMMER_DRKAISER
#endif
#if CONFIG_SATASII == 1
	PROGRAMMER_SATASII
#endif
#if CONFIG_ATAHPT == 1
	PROGRAMMER_ATAHPT
#endif
#if CONFIG_FT2232_SPI == 1
	PROGRAMMER_FT2232_SPI
#endif
#if CONFIG_SERPROG == 1
	PROGRAMMER_SERPROG
#endif
#if CONFIG_BUSPIRATE_SPI == 1
	PROGRAMMER_BUSPIRATE_SPI
#endif
#if CONFIG_DEDIPROG == 1
	PROGRAMMER_DEDIPROG
#endif
#if CONFIG_RAYER_SPI == 1
	PROGRAMMER_RAYER_SPI
#endif
#if CONFIG_NICINTEL == 1
	PROGRAMMER_NICINTEL
#endif
#if CONFIG_NICINTEL_SPI == 1
	PROGRAMMER_NICINTEL_SPI
#endif
#if CONFIG_OGP_SPI == 1
	PROGRAMMER_OGP_SPI
#endif
#if CONFIG_SATAMV == 1
	PROGRAMMER_SATAMV
#endif
#if CONFIG_LINUX_MTD == 1
	PROGRAMMER_LINUX_MTD
#endif
#if CONFIG_LINUX_SPI == 1
	PROGRAMMER_LINUX_SPI
#endif
;
#endif

static void cli_classic_usage(const char *name)
{

	msg_ginfo("Usage: flashrom [-n] [-V] [-f] [-h|-R|-L|"
#if CONFIG_PRINT_WIKI == 1
	         "-z|"
#endif
	         "-E|-r <file>|-w <file>|-v <file>]\n"
	       "       [-i <image>[:<file>]] [-c <chipname>]\n"
	               "[-o <file>] [-l <file>]\n"
	       "       [-p <programmer>[:<parameters>]]\n\n");

	msg_ginfo("Please note that the command line interface for flashrom has "
	         "changed between\n"
	       "0.9.1 and 0.9.2 and will change again before flashrom 1.0.\n"
	       "Do not use flashrom in scripts or other automated tools "
	         "without checking\n"
	       "that your flashrom version won't interpret options in a "
	         "different way.\n\n");

	msg_ginfo("   -h | --help                       print this help text\n"
	       "   -R | --version                    print version (release)\n"
	       "   -r | --read <file|->              read flash and save to "
	         "<file> or write on the standard output\n"
	       "   -w | --write <file|->             write <file> or "
	         "the content provided on the standard input to flash\n"
	       "   -v | --verify <file|->            verify flash against "
	         "<file> or the content provided on the standard input\n"
	       "   -E | --erase                      erase flash device\n"
	       "   -V | --verbose                    more verbose output\n"
	       "   -c | --chip <chipname>            probe only for specified "
	         "flash chip\n"
	       "   -f | --force                      force specific operations "
	         "(see man page)\n"
	       "   -n | --noverify                   don't auto-verify\n"
	       "   -l | --layout <file>              read ROM layout from "
	         "<file>\n"
	       "   -i | --image <name>[:<file>]      only access image <name> "
	         "from flash layout\n"
	       "   -o | --output <name>	             log to file <name>\n"
	       "   -L | --list-supported             print supported devices\n"
	       "   -x | --extract                    extract regions to files\n"
#if CONFIG_PRINT_WIKI == 1
	       "   -z | --list-supported-wiki        print supported devices in wiki syntax\n"
#endif
	       "   -p | --programmer <name>[:<param>] specify the programmer "
	         "device\n"
	);

	list_programmers_linebreak(37, 80, 1);

	msg_ginfo("Long-options:\n"
	       "   --diff <file>                     diff from file instead of ROM\n"
	       "   --do-not-diff                     do not diff with chip"
		  " contents (should be used with erased chips only)\n"
	       "   --fast-verify                     only verify written part\n"
	       "   --flash-name                      flash vendor and device name\n"
	       "   --get-size                        get chip size (bytes)\n"
	       "   --ignore-fmap                     ignore fmap structure\n"
	       "   --ignore-lock                     do not acquire big lock\n"
	       "   --wp-disable                      disable write protection\n"
	       "   --wp-enable                       enable write protection\n"
	       "   --wp-list                         list write protection ranges\n"
	       "   --wp-range <start> <length>       set write protect range\n"
	       "   --wp-region <region>              set write protect range by region name\n"
	       "   --wp-status                       show write protect status\n"
	       );

	msg_ginfo("\nYou can specify one of -h, -R, -L, "
#if CONFIG_PRINT_WIKI == 1
	         "-z, "
#endif
	         "-E, -r, -w, -v or no operation.\n"
	       "If no operation is specified, flashrom will only probe for flash chips.\n");
}

static void cli_classic_abort_usage(const char *msg)
{
	if (msg)
		fprintf(stderr, "%s", msg);
	printf("Please run \"flashrom --help\" for usage info.\n");
	exit(1);
}

static void cli_classic_validate_singleop(int *operation_specified)
{
	if (++(*operation_specified) > 1) {
		cli_classic_abort_usage("More than one operation specified. Aborting.\n");
	}
}

static int check_filename(char *filename, char *type)
{
	if (!filename || (filename[0] == '\0')) {
		fprintf(stderr, "Error: No %s file specified.\n", type);
		return 1;
	}
	/* Not an error, but maybe the user intended to specify a CLI option instead of a file name. */
	if (filename[0] == '-')
		fprintf(stderr, "Warning: Supplied %s file name starts with -\n", type);
	return 0;
}

enum LONGOPT_RETURN_VALUES {
	/* start after ASCII chars */
	LONGOPT_FLASH_SIZE = 256,
	LONGOPT_DIFF,
	LONGOPT_FLASH_NAME,
	LONGOPT_WP_STATUS,
	LONGOPT_WP_SET_RANGE,
	LONGOPT_WP_SET_REGION,
	LONGOPT_WP_ENABLE,
	LONGOPT_WP_DISABLE,
	LONGOPT_WP_LIST,
	LONGOPT_IGNORE_FMAP,
	LONGOPT_FAST_VERIFY,
	LONGOPT_IGNORE_LOCK,
	LONGOPT_DO_NOT_DIFF,
};

int main(int argc, char *argv[])
{
	unsigned long size;
	/* Probe for up to three flash chips. */
	const struct flashchip *chip = NULL;
	struct flashctx flashes[3] = {{0}};
	struct flashctx *fill_flash;
	int startchip = 0;
	int chipcount = 0;
	const char *name;
	int namelen;
	int opt;
	int option_index = 0;
	int force = 0;
	int read_it = 0, write_it = 0, erase_it = 0, verify_it = 0,
		flash_size = 0, dont_verify_it = 0, list_supported = 0,
		extract_it = 0, flash_name = 0, do_diff = 1;
	int set_wp_range = 0, set_wp_region = 0, set_wp_enable = 0,
	    set_wp_disable = 0, wp_status = 0, wp_list = 0;
	int set_ignore_fmap = 0;
#if CONFIG_PRINT_WIKI == 1
	int list_supported_wiki = 0;
#endif
	int operation_specified = 0;
	int i, j;
	enum programmer prog = PROGRAMMER_INVALID;
	int ret = 0;
	int found_chip = 0;

	const char *optstring = "rRwvnVEfc:l:i:p:o:Lzhbx";
	static struct option long_options[] = {
		{"read",		0, 0, 'r'},
		{"write",		0, 0, 'w'},
		{"erase",		0, 0, 'E'},
		{"verify",		0, 0, 'v'},
		{"noverify",		0, 0, 'n'},
		{"chip",		1, 0, 'c'},
		{"verbose",		0, 0, 'V'},
		{"force",		0, 0, 'f'},
		{"layout",		1, 0, 'l'},
		{"image",		1, 0, 'i'},
		{"list-supported",	0, 0, 'L'},
		{"list-supported-wiki", 0, 0, 'z'},
		{"extract", 		0, 0, 'x'},
		{"programmer", 		1, 0, 'p'},
		{"help", 		0, 0, 'h'},
		{"version", 		0, 0, 'R'},
		{"output", 		1, 0, 'o'},
		{"get-size", 		0, 0, LONGOPT_FLASH_SIZE},
		{"flash-size", 		0, 0, LONGOPT_FLASH_SIZE},
		{"flash-name", 		0, 0, LONGOPT_FLASH_NAME},
		{"diff", 		1, 0, LONGOPT_DIFF},
		{"do-not-diff",		0, 0, LONGOPT_DO_NOT_DIFF},
		{"wp-status", 		0, 0, LONGOPT_WP_STATUS},
		{"wp-range", 		0, 0, LONGOPT_WP_SET_RANGE},
		{"wp-region",		1, 0, LONGOPT_WP_SET_REGION},
		{"wp-enable", 		optional_argument, 0, LONGOPT_WP_ENABLE},
		{"wp-disable", 		0, 0, LONGOPT_WP_DISABLE},
		{"wp-list", 		0, 0, LONGOPT_WP_LIST},
		{"ignore-fmap", 	0, 0, LONGOPT_IGNORE_FMAP},
		{"fast-verify",		0, 0, LONGOPT_FAST_VERIFY},
		{"ignore-lock",		0, 0, LONGOPT_IGNORE_LOCK},
		{0, 0, 0, 0}
	};

	char *filename = NULL;
	char *layoutfile = NULL;
	char *diff_file = NULL;
	char *logfile = NULL;
	char *tempstr = NULL;
	char *pparam = NULL;
	char *wp_mode_opt = NULL;
	char *wp_region = NULL;

	print_version();
	print_banner();

	if (selfcheck())
		exit(1);

	setbuf(stdout, NULL);
	/* FIXME: Delay all operation_specified checks until after command
	 * line parsing to allow --help overriding everything else.
	 */
	while ((opt = getopt_long(argc, argv, optstring,
				  long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'r':
			cli_classic_validate_singleop(&operation_specified);
			read_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'w':
			cli_classic_validate_singleop(&operation_specified);
			write_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'v':
			//FIXME: gracefully handle superfluous -v
			cli_classic_validate_singleop(&operation_specified);
			if (dont_verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			if (!verify_it) verify_it = VERIFY_FULL;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'n':
			if (verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			dont_verify_it = 1;
			break;
		case 'c':
			chip_to_probe = strdup(optarg);
			break;
		case 'V':
			verbose_screen++;
			if (verbose_screen > FLASHROM_MSG_DEBUG2)
				verbose_logfile = verbose_screen;
			break;
		case 'E':
			cli_classic_validate_singleop(&operation_specified);
			erase_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'f':
			force = 1;
			break;
		case 'l':
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout specified more than once. Aborting.\n");
			layoutfile = strdup(optarg);
			break;
		case 'i':
			tempstr = strdup(optarg);
			if (register_include_arg(tempstr) < 0) {
				free(tempstr);
				cli_classic_abort_usage(NULL);
			}
			break;
		case 'L':
			cli_classic_validate_singleop(&operation_specified);
			list_supported = 1;
			break;
		case 'x':
			cli_classic_validate_singleop(&operation_specified);
			extract_it = 1;
			break;
		case 'z':
#if CONFIG_PRINT_WIKI == 1
			cli_classic_validate_singleop(&operation_specified);
			list_supported_wiki = 1;
#else
			cli_classic_abort_usage("Error: Wiki output was not"
					"compiled in. Aborting.\n");
#endif
			break;
		case 'p':
			if (prog != PROGRAMMER_INVALID) {
				cli_classic_abort_usage("Error: --programmer specified "
					"more than once. You can separate "
					"multiple\nparameters for a programmer "
					"with \",\". Please see the man page "
					"for details.\n");
			}
			for (prog = 0; prog < PROGRAMMER_INVALID; prog++) {
				name = programmer_table[prog].name;
				namelen = strlen(name);
				if (strncmp(optarg, name, namelen) == 0) {
					switch (optarg[namelen]) {
					case ':':
						pparam = strdup(optarg + namelen + 1);
						if (!strlen(pparam)) {
							free(pparam);
							pparam = NULL;
						}
						break;
					case '\0':
						break;
					default:
						/* The continue refers to the
						 * for loop. It is here to be
						 * able to differentiate between
						 * foo and foobar.
						 */
						continue;
					}
					break;
				}
			}

			for (i = 0; aliases[i].name; i++) {
				name = aliases[i].name;
				namelen = strlen(aliases[i].name);

				if (strncmp(optarg, name, namelen))
					continue;

				switch (optarg[namelen]) {
				case ':':
					pparam = strdup(optarg + namelen + 1);
					if (!strlen(pparam)) {
						free(pparam);
						pparam = NULL;
					}
					break;
				case '\0':
					break;
				default:
					/* The continue refers to the for-loop.
					 * It is here to be able to
					 * differentiate between foo and foobar.
					 */
					continue;
				}

				alias = &aliases[i];
				msg_gdbg("Programmer alias: \"%s\", parameter: "
					" \"%s\",\n", alias->name, pparam);

				break;
			}

			if ((prog == PROGRAMMER_INVALID) && !alias) {
				msg_gerr("Error: Unknown programmer "
					"%s.\n", optarg);
				cli_classic_abort_usage(NULL);
			}

			if ((prog != PROGRAMMER_INVALID) && alias) {
				cli_classic_abort_usage("Error: Alias cannot be used with programmer name.\n");
			}
			break;
		case 'R':
			/* print_version() is always called during startup. */
			cli_classic_validate_singleop(&operation_specified);
			exit(0);
			break;
		case 'h':
			cli_classic_validate_singleop(&operation_specified);
			cli_classic_usage(argv[0]);
			exit(0);
			break;
		case 'o':
#ifdef STANDALONE
			cli_classic_abort_usage("Log file not supported in standalone mode. Aborting.\n");
#else /* STANDALONE */
			logfile = strdup(optarg);
			if (logfile[0] == '\0') {
				cli_classic_abort_usage("No log filename specified.\n");
			}
#endif /* STANDALONE */
			break;
		case LONGOPT_FLASH_SIZE:
			flash_size = 1;
			break;
		case LONGOPT_DO_NOT_DIFF:
			do_diff = 0;
			break;
		case LONGOPT_WP_STATUS:
			wp_status = 1;
			break;
		case LONGOPT_WP_LIST:
			wp_list = 1;
			break;
		case LONGOPT_WP_SET_RANGE:
			set_wp_range = 1;
			break;
		case LONGOPT_WP_SET_REGION:
			set_wp_region = 1;
			wp_region = strdup(optarg);
			break;
		case LONGOPT_WP_ENABLE:
			set_wp_enable = 1;
			if (optarg)
				wp_mode_opt = strdup(optarg);
			break;
		case LONGOPT_WP_DISABLE:
			set_wp_disable = 1;
			break;
		case LONGOPT_FLASH_NAME:
			flash_name = 1;
			break;
		case LONGOPT_DIFF:
			diff_file = strdup(optarg);
			break;
		case LONGOPT_IGNORE_FMAP:
			set_ignore_fmap = 1;
			break;
		case LONGOPT_FAST_VERIFY:
			verify_it = VERIFY_PARTIAL;
			break;
		case LONGOPT_IGNORE_LOCK:
			set_ignore_lock = 1;
			break;
		default:
			cli_classic_abort_usage(NULL);
			break;
		}
	}

#if 0
	if (optind < argc)
		cli_classic_abort_usage("Error: Extra parameter found.\n");
#endif

	if (layoutfile && check_filename(layoutfile, "layout"))
		cli_classic_abort_usage(NULL);


	if (!do_diff && diff_file) {
		cli_classic_abort_usage("Both --diff and --do-not-diff set, what do you want to do?\n");
	}

#ifndef STANDALONE
	if (logfile && check_filename(logfile, "log"))
		cli_classic_abort_usage(NULL);
	if (logfile && open_logfile(logfile))
		cli_classic_abort_usage(NULL);
#endif /* !STANDALONE */

	if (read_it || write_it || verify_it) {
		if (argv[optind])
			filename = argv[optind];
	}
#if CONFIG_PRINT_WIKI == 1
	if (list_supported_wiki) {
		print_supported_wiki();
		exit(0);
	}
#endif

	if (list_supported) {
		if (print_supported())
			ret = 1;
		exit(0);
	}

#ifndef STANDALONE
	start_logging();
#endif /* !STANDALONE */

	print_buildinfo();
	msg_gdbg("Command line (%i args):", argc - 1);
	for (i = 0; i < argc; i++) {
		msg_gdbg(" %s", argv[i]);
	}
	msg_gdbg("\n");

	if (layoutfile && read_romlayout(layoutfile)) {
		cli_classic_abort_usage(NULL);
	}

	/* Does a chip with the requested name exist in the flashchips array? */
	if (chip_to_probe) {
		for (chip = flashchips; chip && chip->name; chip++) {
			if (!strcmp(chip->name, chip_to_probe)) {
				found_chip = 1;
				break;
			}
		}
		for (chip = flashchips_hwseq; chip && chip->name &&
				!found_chip; chip++) {
			if (!strcmp(chip->name, chip_to_probe)) {
				found_chip = 1;
				break;
			}
		}
		if (!found_chip) {
			msg_cerr("Error: Unknown chip '%s' specified.\n", chip_to_probe);
			msg_gerr("Run flashrom -L to view the hardware supported in this flashrom version.\n");
			exit(1);
		}
		/* Keep chip around for later usage in case a forced read is requested. */
	}

	if (prog == PROGRAMMER_INVALID)
		prog = default_programmer;


#if USE_BIG_LOCK == 1
	/* get lock before doing any work that touches hardware */
	if (!set_ignore_lock) {
		msg_gdbg("Acquiring lock (timeout=%d sec)...\n", LOCK_TIMEOUT_SECS);
		if (acquire_big_lock(LOCK_TIMEOUT_SECS) < 0) {
			msg_gerr("Could not acquire lock.\n");
			exit(1);
		}
		msg_gdbg("Lock acquired.\n");
	}
#endif

	/*
	 * Let powerd know that we're updating firmware so machine stays awake.
	 *
	 * A bit of history behind this small block of code:
	 * chromium-os:15025 - If broken_timer == 1, use busy loop instead of
	 * OS timers to avoid excessive usleep overhead during "long" operations
	 * involving reads, erases, and writes. This was mostly a problem on
	 * old machines with poor DVFS implementations.
	 *
	 * chromium-os:18895 - Disabled power management to prevent system from
	 * going to sleep while doing a destructive operation.
	 *
	 * chromium-os:19321 - Use OS timers for non-destructive operations to
	 * avoid UI jank.
	 *
	 * chromium:400641 - Powerd is smarter now, so instead of stopping it
	 * manually we'll use a file lock so it knows not to put the machine
	 * to sleep or do other things that can interfere.
	 *
	 */
	if (write_it || erase_it)
		disable_power_management();

	/* FIXME: Delay calibration should happen in programmer code. */
	myusec_calibrate_delay();

	if (programmer_init(prog, pparam)) {
		msg_perr("Error: Programmer initialization failed.\n");
		ret = 1;
		goto out_shutdown;
	}

	// FIXME(quasisec): Hack to loop correctly while we have no actual
	// registered masters. Remove once we use new dispatch mechanism!
	const struct registered_master mst_nop;
	if (!registered_master_count) register_master(&mst_nop);

	tempstr = flashbuses_to_text(get_buses_supported());
	msg_pdbg("The following protocols are supported: %s.\n", tempstr);
	free(tempstr);
	tempstr = NULL;

	for (j = 0; j < registered_master_count; j++) {
		while (chipcount < (int)ARRAY_SIZE(flashes)) {
			startchip = probe_flash(&registered_masters[j], startchip, &flashes[chipcount], 0);
			if (startchip == -1)
				break;
			chipcount++;
			startchip++;
		}
	}

	if (chipcount > 1) {
		msg_cinfo("Multiple flash chip definitions match the detected chip(s): \"%s\"",
			  flashes[0].chip->name);
		for (i = 1; i < chipcount; i++)
			msg_cinfo(", \"%s\"", flashes[i].chip->name);
		msg_cinfo("\nPlease specify which chip definition to use with the -c <chipname> option.\n");
		ret = 1;
		goto out_shutdown;
	} else if (!chipcount) {
		msg_cinfo("No EEPROM/flash device found.\n");
		if (!force || !chip_to_probe) {
			msg_cinfo("Note: flashrom can never write if the flash chip isn't found "
				  "automatically.\n");
		}
		if (force && read_it && chip_to_probe) {
			msg_ginfo("Force read (-f -r -c) requested, pretending the chip is there:\n");
			// FIXME(quasisec): Passing in NULL for registered_master as we don't know how to handle
			// the case of a forced chip with multiple compatible programmers that are registered.
			startchip = probe_flash(NULL, 0, &flashes[0], 1);
			if (startchip == -1) {
				// FIXME: This should never happen! Ask for a bug report?
				msg_cinfo("Probing for flash chip '%s' failed.\n", chip_to_probe);
				ret = 1;
				goto out_shutdown;
			}
			msg_cinfo("Please note that forced reads most likely contain garbage.\n");
			return read_flash_to_file(&flashes[0], filename);
		}
		ret = 1;
		goto out_shutdown;
	}

	fill_flash = &flashes[0];

	print_chip_support_status(fill_flash->chip);

	size = fill_flash->chip->total_size * 1024;
	if (check_max_decode((buses_supported & fill_flash->chip->bustype), size) &&
	    (!force)) {
		msg_gerr("Chip is too big for this programmer "
			"(-V gives details). Use --force to override.\n");
		ret = 1;
		goto out_shutdown;
	}

	if (!(read_it | write_it | verify_it | erase_it | flash_name |
	      flash_size | set_wp_range | set_wp_region | set_wp_enable |
	      set_wp_disable | wp_status | wp_list | extract_it)) {
		msg_gerr("No operations were specified.\n");
		// FIXME: flash writes stay enabled!
		ret = 0;
		goto out_shutdown;
	}

	if (set_wp_enable && set_wp_disable) {
		msg_ginfo("Error: --wp-enable and --wp-disable are mutually exclusive\n");
		ret = 1;
		goto out_shutdown;
	}

	/*
	 * Common rules for -r/-w/-v syntax parsing:
	 * - If no filename is specified at all, quit.
	 * - If no filename is specified for -r/-w/-v, but files are specified
	 *   for -i, then the number of file arguments for -i options must be
	 *   equal to the total number of -i options.
	 *
	 * Rules for reading:
	 * - If files are specified for -i args but not -r, do partial reads for
	 *   each -i arg, creating a new file for each region. Each -i option
	 *   must specify a filename.
	 * - If filenames are specified for -r and -i args, then:
	 *     - Do partial read for each -i arg, creating a new file for
	 *       each region where a filename is provided (-i region:filename).
	 *     - Create a ROM-sized file with partially filled content. For each
	 *       -i arg, fill the corresponding offset with content from ROM.
	 *
	 * Rules for writing and verifying:
	 * - If files are specified for both -w/-v and -i args, -i files take
	 *   priority. (Note: We determined this was the most useful syntax for
	 *   chromium.org's flashrom after some discussion. Upstream may wish
	 *   to quit in this case due to ambiguity).
	 *   See: http://crbug.com/263495.
	 * - If file is specified for -w/-v and no files are specified with -i
	 *   args, then the file is to be used for writing/verifying the entire
	 *   ROM.
	 * - If files are specified for -i args but not -w, do partial writes
	 *   for each -i arg. Likewise for -v and -i args. All -i args must
	 *   supply a filename. Any omission is considered ambiguous.
	 * - Regions with a filename associated must not overlap. This is also
	 *   considered ambiguous. Note: This is checked later since it requires
	 *   processing the layout/fmap first.
	 */
	if (read_it || write_it || verify_it) {
		char op;

		if (read_it)
			op = 'r';
		else if (write_it)
			op = 'w';
		else if (verify_it)
			op = 'v';
		else {
			msg_gerr("Error: Unknown file operation\n");
			ret = 1;
			goto out_shutdown;
		}

		if (!filename) {
			if (!get_num_include_args()) {
				msg_gerr("Error: No file specified for -%c.\n",
						op);
				ret = 1;
				goto out_shutdown;
			}

			if (num_include_files() != get_num_include_args()) {
				msg_gerr("Error: One or more -i arguments is "
					" missing a filename.\n");
				ret = 1;
				goto out_shutdown;
			}
		}
	}

	/* Always verify write operations unless -n is used. */
	if (write_it && !dont_verify_it)
		if (!verify_it) verify_it = VERIFY_FULL;

	/* Note: set_wp_disable should be done before setting the range */
	if (set_wp_disable) {
		if (fill_flash->chip->wp && fill_flash->chip->wp->disable) {
			ret |= fill_flash->chip->wp->disable(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			ret = 1;
			goto out_shutdown;
		}
	}

	if (flash_name) {
		if (fill_flash->chip->vendor && fill_flash->chip->name) {
			printf("vendor=\"%s\" name=\"%s\"\n",
				fill_flash->chip->vendor,
				fill_flash->chip->name);
		} else {
			ret = -1;
		}
		goto out_shutdown;
	}

	/* If the user doesn't specify any -i argument, then we can skip the
	 * fmap parsing to speed up. */
	if (get_num_include_args() == 0 && !extract_it) {
		msg_gdbg("No -i argument is specified, set ignore_fmap.\n");
		set_ignore_fmap = 1;
	}

	/*
	 * Add entries for regions specified in flashmap, unless the user
	 * explicitly requested not to look for fmap, or provided a layout
	 * file.
	 */
	if (!set_ignore_fmap && !layoutfile &&
	    get_fmap_entries(filename, fill_flash) < 0) {
		ret = 1;
		goto out_shutdown;
	}

	if (set_wp_range || set_wp_region) {
		if (set_wp_range && set_wp_region) {
			msg_gerr("Error: Cannot use both --wp-range and "
				"--wp-region simultaneously.\n");
			ret = 1;
			goto out_shutdown;
		}

		if (!fill_flash->chip->wp || !fill_flash->chip->wp->set_range) {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			ret = 1;
			goto out_shutdown;
		}
	}

	/* Note: set_wp_range must happen before set_wp_enable */
	if (set_wp_range) {
		unsigned int start, len;
		char *endptr = NULL;

		if ((argc - optind) != 2) {
			msg_gerr("Error: invalid number of arguments\n");
			ret = 1;
			goto out_shutdown;
		}

		/* FIXME: add some error checking */
		start = strtoul(argv[optind], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			msg_gerr("Error: value \"%s\" invalid\n", argv[optind]);
			ret = 1;
			goto out_shutdown;
		}

		len = strtoul(argv[optind + 1], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			msg_gerr("Error: value \"%s\" invalid\n", argv[optind + 1]);
			ret = 1;
			goto out_shutdown;
		}

		ret |= fill_flash->chip->wp->set_range(fill_flash, start, len);
	}

	if (set_wp_region && wp_region) {
		int n;
		struct romentry entry;

		n = find_romentry(wp_region);
		if (n < 0) {
			msg_gerr("Error: Unable to find region \"%s\"\n",
					wp_region);
			ret = 1;
			goto out_shutdown;
		}

		if (fill_romentry(&entry, n)) {
			ret = 1;
			goto out_shutdown;
		}

		ret |= fill_flash->chip->wp->set_range(fill_flash,
				entry.start, entry.end - entry.start + 1);
		free(wp_region);
	}

	if (!ret && set_wp_enable) {
		enum wp_mode wp_mode;

		if (wp_mode_opt)
			wp_mode = get_wp_mode(wp_mode_opt);
		else
			wp_mode = WP_MODE_HARDWARE;	/* default */

		if (wp_mode == WP_MODE_UNKNOWN) {
			msg_gerr("Error: Invalid WP mode: \"%s\"\n", wp_mode_opt);
			ret = 1;
			goto out_shutdown;
		}

		if (fill_flash->chip->wp && fill_flash->chip->wp->enable) {
			ret |= fill_flash->chip->wp->enable(fill_flash, wp_mode);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			ret = 1;
			goto out_shutdown;
		}
	}

	if (flash_size) {
		msg_ginfo("%d\n", fill_flash->chip->total_size * 1024);
		goto out_shutdown;
	}

	if (wp_status) {
		if (fill_flash->chip->wp && fill_flash->chip->wp->wp_status) {
			ret |= fill_flash->chip->wp->wp_status(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			ret = 1;
		}
		goto out_shutdown;
	}

	if (wp_list) {
		msg_ginfo("Valid write protection ranges:\n");
		if (fill_flash->chip->wp && fill_flash->chip->wp->list_ranges) {
			ret |= fill_flash->chip->wp->list_ranges(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			ret = 1;
		}
		goto out_shutdown;
	}

	if (read_it || write_it || erase_it || verify_it || extract_it) {
		ret = doit(fill_flash, force, filename,
		          read_it, write_it, erase_it, verify_it,
		          extract_it, diff_file, do_diff);
	}

	msg_ginfo("%s\n", ret ? "FAILED" : "SUCCESS");
out_shutdown:
	programmer_shutdown();  /* must be done after chip_restore() */
#if USE_BIG_LOCK == 1
	if (!set_ignore_lock)
		release_big_lock();
#endif
	if (restore_power_management()) {
		msg_gerr("Unable to re-enable power management\n");
		ret |= 1;
	}

	layout_cleanup();
#ifndef STANDALONE
	ret |= close_logfile();
#endif /* !STANDALONE */
	return ret;
}
