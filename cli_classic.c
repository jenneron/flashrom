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

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <getopt.h>
#include "big_lock.h"
#include "flash.h"
#include "flashchips.h"
#include "fmap.h"
#include "power.h"
#include "programmer.h"
#include "writeprotect.h"

#define LOCK_TIMEOUT_SECS	180

int set_ignore_lock = 0;

#include "libflashrom.h"

static void cli_classic_usage(const char *name)
{
	printf("Usage: %s [-h|-R|-L|"
#if CONFIG_PRINT_WIKI == 1
	       "-z|"
#endif
	       "\n\t-p <programmername>[:<parameters>] [-c <chipname>]\n"
	       "\t\t(--flash-name|--flash-size|\n"
	       "\t\t [-E|-x|(-r|-w|-v) [<file>]]\n"
	       "\t\t [(-l <layoutfile>|--ifd|--fmap|--fmap-file <fmapfile>) [-i <region>[:<file>]]...]\n"
	       "\t\t [-n] [-N] [-f])]\n"
	       "\t[-V[V[V]]] [-o <logfile>]\n\n", name);

	printf(" -h | --help                        print this help text\n"
	       " -R | --version                     print version (release)\n"
	       " -r | --read [<file>]               read flash and save to <file>\n"
	       " -w | --write [<file|->]            write <file> or the content provided\n"
	       "                                    on the standard input to flash\n"
	       " -v | --verify [<file|->]           verify flash against <file>\n"
	       "                                    or the content provided on the standard input\n"
	       " -E | --erase                       erase flash memory\n"
	       " -V | --verbose                     more verbose output\n"
	       " -c | --chip <chipname>             probe only for specified flash chip\n"
	       " -f | --force                       force specific operations (see man page)\n"
	       " -n | --noverify                    don't auto-verify\n"
	       " -N | --noverify-all                verify included regions only (cf. -i)\n"
	       " -x | --extract                     extract regions to files\n"
	       " -l | --layout <layoutfile>         read ROM layout from <layoutfile>\n"
	       "      --wp-disable                  disable write protection\n"
	       "      --wp-enable                   enable write protection\n"
	       "      --wp-list                     list write protect range\n"
	       "      --wp-status                   show write protect status\n"
	       "      --wp-range=<start>,<len>      set write protect range\n"
	       "      --wp-region <region>          set write protect region\n"
	       "      --flash-name                  read out the detected flash name\n"
	       "      --flash-size                  read out the detected flash size\n"
	       "      --fmap                        read ROM layout from fmap embedded in ROM\n"
	       "      --fmap-file <fmapfile>        read ROM layout from fmap in <fmapfile>\n"
	       "      --ifd                         read layout from an Intel Firmware Descriptor\n"
	       " -i | --image <region>[:<file>]     only read/write image <region> from layout\n"
	       "                                    (optionally with data from <file>)\n"
	       " -o | --output <logfile>            log output to <logfile>\n"
	       "      --flash-contents <ref-file>   assume flash contents to be <ref-file>\n"
	       "      --do-not-diff                 do not diff with chip contents\n"
	       "                                    (should be used with erased chips only)\n"
	       "      --ignore-lock                 do not acquire big lock\n"
	       " -L | --list-supported              print supported devices\n"
#if CONFIG_PRINT_WIKI == 1
	       " -z | --list-supported-wiki         print supported devices in wiki syntax\n"
#endif
	       " -p | --programmer <name>[:<param>] specify the programmer device. One of\n");
	list_programmers_linebreak(4, 80, 0);
	printf(".\n\nYou can specify one of -h, -R, -L, "
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

static int check_filename(char *filename, const char *type)
{
	if (!filename || (filename[0] == '\0')) {
		fprintf(stderr, "Error: No %s file specified.\n", type);
		return 1;
	}
	/* Not an error, but maybe the user intended to specify a CLI option instead of a file name. */
	if (filename[0] == '-' && filename[1] != '\0')
		fprintf(stderr, "Warning: Supplied %s file name starts with -\n", type);
	return 0;
}

/* Ensure a file is open by means of fstat */
static bool check_file(FILE *file)
{
#ifndef STANDALONE
	struct stat statbuf;

	if (fstat(fileno(file), &statbuf) < 0)
		return false;
#endif /* !STANDALONE */
	return true;
}

static int parse_wp_range(unsigned int *start, unsigned int *len)
{
	char *endptr = NULL, *token = NULL;

	if (!optarg) {
		msg_gerr("Error: No wp-range values provided\n");
		return -1;
	}

	token = strtok(optarg, ",");
	if (!token) {
		msg_gerr("Error: Invalid wp-range argument format\n");
		return -1;
	}
	*start = strtoul(token, &endptr, 0);

	token = strtok(NULL, ",");
	if (!token) {
		msg_gerr("Error: Invalid wp-range argument format\n");
		return -1;
	}
	*len = strtoul(token, &endptr, 0);

	return 0;
}

static char *get_optional_filename(char *argv[])
{
	char *filename = NULL;

	/* filename was supplied in optarg (i.e. -rfilename) */
	if (optarg != NULL)
		filename = strdup(optarg);
	/* filename is on optind if it is not another flag (i.e. -r filename)
	 * - is treated as stdin, so we still strdup in this case
	 */
	else if (optarg == NULL && argv[optind] != NULL &&
		 (argv[optind][0] != '-' || argv[optind][1] == '\0'))
		filename = strdup(argv[optind++]);

	return filename;
}

static int flashrom_layout_read_fmap_from_file(struct flashrom_layout **layout,
					       struct flashrom_flashctx *flashctx, const char *fmapfile)
{
	int ret = 1;
	struct stat s;
	if (stat(fmapfile, &s) != 0) {
		return ret;
	}

	size_t fmapfile_size = s.st_size;
	uint8_t *fmapfile_buffer = malloc(fmapfile_size);
	if (!fmapfile_buffer) {
		return ret;
	}

	if (read_buf_from_file(fmapfile_buffer, fmapfile_size, fmapfile)) {
		goto out;
	}

	ret = flashrom_layout_read_fmap_from_buffer(layout, flashctx, fmapfile_buffer, fmapfile_size);
out:
	free(fmapfile_buffer);
	return ret;
}

int main(int argc, char *argv[])
{
	const struct flashchip *chip = NULL;
	/* Probe for up to eight flash chips. */
	struct flashctx flashes[8] = {{0}};
	struct flashctx *fill_flash;
	const char *name;
	int namelen, opt, i, j;
	int startchip = -1, chipcount = 0, option_index = 0, force = 0, ifd = 0, fmap = 0;
#if CONFIG_PRINT_WIKI == 1
	int list_supported_wiki = 0;
#endif
	int flash_name = 0, flash_size = 0;
	int set_wp_enable = 0, set_wp_disable = 0, wp_status = 0;
	int set_wp_range = 0, set_wp_region = 0, wp_list = 0;
	int read_it = 0, extract_it = 0, write_it = 0, erase_it = 0, verify_it = 0;
	int dont_verify_it = 0, dont_verify_all = 0, list_supported = 0, operation_specified = 0;
	int do_not_diff = 0;
	struct flashrom_layout *layout = NULL;
	enum programmer prog = PROGRAMMER_INVALID;
	enum {
		OPTION_IFD = 0x0100,
		OPTION_FMAP,
		OPTION_FMAP_FILE,
		OPTION_FLASH_CONTENTS,
		OPTION_FLASH_NAME,
		OPTION_FLASH_SIZE,
		OPTION_WP_STATUS,
		OPTION_WP_SET_RANGE,
		OPTION_WP_SET_REGION,
		OPTION_WP_ENABLE,
		OPTION_WP_DISABLE,
		OPTION_WP_LIST,
		OPTION_DO_NOT_DIFF,
		OPTION_IGNORE_LOCK,
	};
	int ret = 0;
	unsigned int wp_start = 0, wp_len = 0;

	static const char optstring[] = "r::Rw::v::nNVEfc:l:i:p:Lzho:x";
	static const struct option long_options[] = {
		{"read",		2, NULL, 'r'},
		{"write",		2, NULL, 'w'},
		{"erase",		0, NULL, 'E'},
		{"verify",		2, NULL, 'v'},
		{"noverify",		0, NULL, 'n'},
		{"noverify-all",	0, NULL, 'N'},
		{"extract",		0, NULL, 'x'},
		{"chip",		1, NULL, 'c'},
		{"verbose",		0, NULL, 'V'},
		{"force",		0, NULL, 'f'},
		{"layout",		1, NULL, 'l'},
		{"ifd",			0, NULL, OPTION_IFD},
		{"fmap",		0, NULL, OPTION_FMAP},
		{"fmap-file",		1, NULL, OPTION_FMAP_FILE},
		{"image",		1, NULL, 'i'},
		{"flash-contents",	1, NULL, OPTION_FLASH_CONTENTS},
		{"flash-name",		0, NULL, OPTION_FLASH_NAME},
		{"flash-size",		0, NULL, OPTION_FLASH_SIZE},
		{"get-size",		0, NULL, OPTION_FLASH_SIZE}, // (deprecated): back compatibility.
		{"wp-status", 		0, 0, OPTION_WP_STATUS},
		{"wp-range", 		required_argument, NULL, OPTION_WP_SET_RANGE},
		{"wp-region",		1, 0, OPTION_WP_SET_REGION},
		{"wp-enable", 		optional_argument, 0, OPTION_WP_ENABLE},
		{"wp-disable", 		0, 0, OPTION_WP_DISABLE},
		{"wp-list", 		0, 0, OPTION_WP_LIST},
		{"list-supported",	0, NULL, 'L'},
		{"list-supported-wiki",	0, NULL, 'z'},
		{"programmer",		1, NULL, 'p'},
		{"help",		0, NULL, 'h'},
		{"version",		0, NULL, 'R'},
		{"output",		1, NULL, 'o'},
		{"do-not-diff",		0, 0, OPTION_DO_NOT_DIFF},
		{"ignore-lock",		0, 0, OPTION_IGNORE_LOCK},
		{NULL,			0, NULL, 0},
	};

	char *filename = NULL;
	char *referencefile = NULL;
	char *layoutfile = NULL;
	char *fmapfile = NULL;
#ifndef STANDALONE
	char *logfile = NULL;
#endif /* !STANDALONE */
	char *tempstr = NULL;
	char *pparam = NULL;
	struct layout_include_args *include_args = NULL;
	char *wp_mode_opt = NULL;
	char *wp_region = NULL;

	/*
	 * Safety-guard against a user who has (mistakenly) closed
	 * stdout or stderr before exec'ing flashrom.  We disable
	 * logging in this case to prevent writing log data to a flash
	 * chip when a flash device gets opened with fd 1 or 2.
	 */
	if (check_file(stdout) && check_file(stderr)) {
		flashrom_set_log_callback(
			(flashrom_log_callback *)&flashrom_print_cb);
	}

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
			filename = get_optional_filename(argv);
			read_it = 1;
			break;
		case 'w':
			cli_classic_validate_singleop(&operation_specified);
			filename = get_optional_filename(argv);
			write_it = 1;
			break;
		case 'v':
			//FIXME: gracefully handle superfluous -v
			cli_classic_validate_singleop(&operation_specified);
			if (dont_verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			filename = get_optional_filename(argv);
			verify_it = 1;
			break;
		case 'n':
			if (verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			dont_verify_it = 1;
			break;
		case 'N':
			dont_verify_all = 1;
			break;
		case 'x':
			cli_classic_validate_singleop(&operation_specified);
			extract_it = 1;
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
			break;
		case 'f':
			force = 1;
			break;
		case 'l':
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout specified more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --layout and --ifd both specified. Aborting.\n");
			if (fmap)
				cli_classic_abort_usage("Error: --layout and --fmap-file both specified. Aborting.\n");
			layoutfile = strdup(optarg);
			break;
		case OPTION_IFD:
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout and --ifd both specified. Aborting.\n");
			if (fmap)
				cli_classic_abort_usage("Error: --fmap-file and --ifd both specified. Aborting.\n");
			ifd = 1;
			break;
		case OPTION_FMAP_FILE:
			if (fmap)
				cli_classic_abort_usage("Error: --fmap or --fmap-file specified "
					"more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --fmap-file and --ifd both specified. Aborting.\n");
			if (layoutfile)
				cli_classic_abort_usage("Error: --fmap-file and --layout both specified. Aborting.\n");
			fmapfile = strdup(optarg);
			fmap = 1;
			break;
		case OPTION_FMAP:
			if (fmap)
				cli_classic_abort_usage("Error: --fmap or --fmap-file specified "
					"more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --fmap and --ifd both specified. Aborting.\n");
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout and --fmap both specified. Aborting.\n");
			fmap = 1;
			break;
		case 'i':
			if (register_include_arg(&include_args, optarg))
				cli_classic_abort_usage(NULL);
			break;
		case OPTION_FLASH_CONTENTS:
			if (referencefile)
				cli_classic_abort_usage("Error: --flash-contents specified more than once."
							"Aborting.\n");
			if (do_not_diff)
				cli_classic_abort_usage("Error: --flash-contents and --do-not-diff both "
							"specified. Aborting.\n");
			referencefile = strdup(optarg);
			break;
		case OPTION_FLASH_NAME:
			cli_classic_validate_singleop(&operation_specified);
			flash_name = 1;
			break;
		case OPTION_FLASH_SIZE:
			cli_classic_validate_singleop(&operation_specified);
			flash_size = 1;
			break;
		case OPTION_WP_STATUS:
			wp_status = 1;
			break;
		case OPTION_WP_LIST:
			wp_list = 1;
			break;
		case OPTION_WP_SET_RANGE:
			if (parse_wp_range(&wp_start, &wp_len) < 0)
				cli_classic_abort_usage("Incorrect wp-range arguments provided.\n");

			set_wp_range = 1;
			break;
		case OPTION_WP_ENABLE:
			set_wp_enable = 1;
			if (optarg)
				wp_mode_opt = strdup(optarg);
			break;
		case OPTION_WP_DISABLE:
			set_wp_disable = 1;
			break;
		case 'L':
			cli_classic_validate_singleop(&operation_specified);
			list_supported = 1;
			break;
		case 'z':
#if CONFIG_PRINT_WIKI == 1
			cli_classic_validate_singleop(&operation_specified);
			list_supported_wiki = 1;
#else
			cli_classic_abort_usage("Error: Wiki output was not "
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
				name = programmer_table[prog]->name;
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
			if (prog == PROGRAMMER_INVALID) {
				fprintf(stderr, "Error: Unknown programmer \"%s\". Valid choices are:\n",
					optarg);
				list_programmers_linebreak(0, 80, 0);
				msg_ginfo(".\n");
				cli_classic_abort_usage(NULL);
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
			if (logfile) {
				fprintf(stderr, "Warning: -o/--output specified multiple times.\n");
				free(logfile);
			}

			logfile = strdup(optarg);
			if (logfile[0] == '\0') {
				cli_classic_abort_usage("No log filename specified.\n");
			}
#endif /* STANDALONE */
			break;
		case OPTION_DO_NOT_DIFF:
			if (referencefile)
				cli_classic_abort_usage("Error: --flash-contents and --do-not-diff both "
							"specified. Aborting.\n");
			do_not_diff = 1;
			break;
		case OPTION_WP_SET_REGION:
			set_wp_region = 1;
			wp_region = strdup(optarg);
			break;
		case OPTION_IGNORE_LOCK:
			set_ignore_lock = 1;
			break;
		default:
			cli_classic_abort_usage(NULL);
			break;
		}
	}

	if (optind < argc)
		cli_classic_abort_usage("Error: Extra parameter found.\n");
	if (filename && check_filename(filename, "image"))
		cli_classic_abort_usage(NULL);
	if (layoutfile && check_filename(layoutfile, "layout"))
		cli_classic_abort_usage(NULL);
	if (fmapfile && check_filename(fmapfile, "fmap"))
		cli_classic_abort_usage(NULL);
	if (referencefile && check_filename(referencefile, "reference"))
		cli_classic_abort_usage(NULL);

#ifndef STANDALONE
	if (logfile && check_filename(logfile, "log"))
		cli_classic_abort_usage(NULL);
	if (logfile && open_logfile(logfile))
		cli_classic_abort_usage(NULL);
#endif /* !STANDALONE */

#if CONFIG_PRINT_WIKI == 1
	if (list_supported_wiki) {
		print_supported_wiki();
		goto out;
	}
#endif

	if (list_supported) {
		if (print_supported())
			ret = 1;
		goto out;
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
		ret = 1;
		goto out;
	}

	/* If the user specifies a -i argument and no layout, then we do fmap
	 * parsing. */
	if ((include_args || extract_it) && !layoutfile && !ifd) {
		msg_gdbg("-i argument specified, set fmap.\n");
		fmap = 1;
	}

	if (!ifd && !fmap && process_include_args(get_global_layout(), include_args)) {
		ret = 1;
		goto out;
	}
	/* Does a chip with the requested name exist in the flashchips array? */
	if (chip_to_probe) {
		for (chip = flashchips; chip && chip->name; chip++)
			if (!strcmp(chip->name, chip_to_probe))
				break;
		if (!chip || !chip->name) {
			msg_cerr("Error: Unknown chip '%s' specified.\n", chip_to_probe);
			msg_gerr("Run flashrom -L to view the hardware supported in this flashrom version.\n");
			ret = 1;
			goto out;
		}
		/* Keep chip around for later usage in case a forced read is requested. */
	}

	if (prog == PROGRAMMER_INVALID) {
		if (CONFIG_DEFAULT_PROGRAMMER != PROGRAMMER_INVALID) {
			prog = CONFIG_DEFAULT_PROGRAMMER;
			/* We need to strdup here because we free(pparam) unconditionally later. */
			pparam = strdup(CONFIG_DEFAULT_PROGRAMMER_ARGS);
			msg_pinfo("Using default programmer \"%s\" with arguments \"%s\".\n",
				  programmer_table[CONFIG_DEFAULT_PROGRAMMER]->name, pparam);
		} else {
			msg_perr("Please select a programmer with the --programmer parameter.\n"
#if CONFIG_INTERNAL == 1
				 "To choose the mainboard of this computer use 'internal'. "
#endif
				 "Valid choices are:\n");
			list_programmers_linebreak(0, 80, 0);
			msg_ginfo(".\n");
			ret = 1;
			goto out;
		}
	}

#if USE_BIG_LOCK == 1
	/* get lock before doing any work that touches hardware */
	if (!set_ignore_lock) {
		msg_gdbg("Acquiring lock (timeout=%d sec)...\n", LOCK_TIMEOUT_SECS);
		if (acquire_big_lock(LOCK_TIMEOUT_SECS) < 0) {
			msg_gerr("Could not acquire lock.\n");
			ret = 1;
			goto out;
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
	tempstr = flashbuses_to_text(get_buses_supported());
	msg_pdbg("The following protocols are supported: %s.\n", tempstr);
	free(tempstr);
	tempstr = NULL;

	for (j = 0; j < registered_master_count; j++) {
		startchip = 0;
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
			struct registered_master *mst;
			int compatible_masters = 0;
			msg_cinfo("Force read (-f -r -c) requested, pretending the chip is there:\n");
			/* This loop just counts compatible controllers. */
			for (j = 0; j < registered_master_count; j++) {
				mst = &registered_masters[j];
				/* chip is still set from the chip_to_probe earlier in this function. */
				if (mst->buses_supported & chip->bustype)
					compatible_masters++;
			}
			if (!compatible_masters) {
				msg_cinfo("No compatible controller found for the requested flash chip.\n");
				ret = 1;
				goto out_shutdown;
			}
			if (compatible_masters > 1)
				msg_cinfo("More than one compatible controller found for the requested flash "
					  "chip, using the first one.\n");
			for (j = 0; j < registered_master_count; j++) {
				mst = &registered_masters[j];
				startchip = probe_flash(mst, 0, &flashes[0], 1);
				if (startchip != -1)
					break;
			}
			if (startchip == -1) {
				// FIXME: This should never happen! Ask for a bug report?
				msg_cinfo("Probing for flash chip '%s' failed.\n", chip_to_probe);
				ret = 1;
				goto out_shutdown;
			}
			if (map_flash(&flashes[0]) != 0) {
				free(flashes[0].chip);
				ret = 1;
				goto out_shutdown;
			}
			msg_cinfo("Please note that forced reads most likely contain garbage.\n");
			ret = read_flash_to_file(&flashes[0], filename);
			unmap_flash(&flashes[0]);
			free(flashes[0].chip);
			goto out_shutdown;
		}
		ret = 1;
		goto out_shutdown;
	} else if (!chip_to_probe) {
		/* repeat for convenience when looking at foreign logs */
		tempstr = flashbuses_to_text(flashes[0].chip->bustype);
		msg_gdbg("Found %s flash chip \"%s\" (%d kB, %s).\n",
			 flashes[0].chip->vendor, flashes[0].chip->name, flashes[0].chip->total_size, tempstr);
		free(tempstr);
	}

	fill_flash = &flashes[0];

	print_chip_support_status(fill_flash->chip);

	unsigned int limitexceeded = count_max_decode_exceedings(fill_flash);
	if (limitexceeded > 0 && !force) {
		enum chipbustype commonbuses = fill_flash->mst->buses_supported & fill_flash->chip->bustype;

		/* Sometimes chip and programmer have more than one bus in common,
		 * and the limit is not exceeded on all buses. Tell the user. */
		if ((bitcount(commonbuses) > limitexceeded)) {
			msg_pdbg("There is at least one interface available which could support the size of\n"
				 "the selected flash chip.\n");
		}
		msg_cerr("This flash chip is too big for this programmer (--verbose/-V gives details).\n"
			 "Use --force/-f to override at your own risk.\n");
		ret = 1;
		goto out_shutdown;
	}

	if (!(read_it | write_it | verify_it | erase_it | flash_name | flash_size
	      | set_wp_range | set_wp_region | set_wp_enable |
	      set_wp_disable | wp_status | wp_list | extract_it)) {
		msg_ginfo("No operations were specified.\n");
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
	 *   priority.
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
	if ((read_it | write_it | verify_it) && !filename) {
		struct layout_include_args *arg;
		if (!include_args) {
			msg_gerr("Error: No image file specified.\n");
			ret = 1;
			goto out_shutdown;
		}

		for (arg = include_args; arg; arg = arg->next) {
			if (check_filename(arg->file, "region")) {
				ret = 1;
				goto out_shutdown;
			}
		}
	}

	if (set_wp_enable && set_wp_disable) {
		msg_ginfo("Error: --wp-enable and --wp-disable are mutually exclusive\n");
		ret = 1;
		goto out_shutdown;
	}
	if (set_wp_range && set_wp_region) {
		msg_gerr("Error: Cannot use both --wp-range and --wp-region simultaneously.\n");
		ret = 1;
		goto out_shutdown;
	}

	struct wp *wp = get_wp_for_flashchip(fill_flash->chip);
	if (set_wp_range || set_wp_region) {
		if (!wp || !wp->set_range) {
			msg_gerr("Error: write protect is not supported on this flash chip.\n");
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

	if (flash_size) {
		printf("%d\n", fill_flash->chip->total_size * 1024);
		goto out_shutdown;
	}

	if (layoutfile) {
		layout = get_global_layout();
	} else if (ifd && (flashrom_layout_read_from_ifd(&layout, fill_flash, NULL, 0) ||
			   process_include_args(layout, include_args))) {
		ret = 1;
		goto out_shutdown;
	} else if (fmap && fmapfile &&
		   (flashrom_layout_read_fmap_from_file(&layout, fill_flash, fmapfile) ||
		    process_include_args(layout, include_args))) {
		ret = 1;
		goto out_shutdown;
	} else if (!ifd && fmap &&
		   ((flashrom_layout_read_fmap_from_file(&layout, fill_flash, filename) &&
		     flashrom_layout_read_fmap_from_rom(&layout, fill_flash, 0,
							fill_flash->chip->total_size * 1024)) ||
		    process_include_args(layout, include_args))) {
		ret = 1;
		goto out_shutdown;
	}
	flashrom_layout_set(fill_flash, layout);

	if (wp_status) {
		if (wp && wp->wp_status) {
			ret |= wp->wp_status(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported on this flash chip.\n");
			ret = 1;
		}
		goto out_release;
	}

	/* Note: set_wp_disable should be done before setting the range */
	if (set_wp_disable) {
		if (wp && wp->disable) {
			ret |= wp->disable(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported on this flash chip.\n");
			ret = 1;
			goto out_release;
		}
	}

	/* Note: set_wp_range must happen before set_wp_enable */
	if (set_wp_range) {
		ret |= wp->set_range(fill_flash, wp_start, wp_len);
	}

	if (set_wp_region && wp_region) {
		if (get_region_range(layout, wp_region, &wp_start, &wp_len)) {
			ret = 1;
			goto out_release;
		}
		ret |= wp->set_range(fill_flash, wp_start, wp_len);
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
			goto out_release;
		}

		if (wp && wp->enable) {
			ret |= wp->enable(fill_flash, wp_mode);
		} else {
			msg_gerr("Error: write protect is not supported on this flash chip.\n");
			ret = 1;
			goto out_release;
		}
	}

	if (wp_list) {
		msg_ginfo("Valid write protection ranges:\n");
		if (wp && wp->list_ranges) {
			ret |= wp->list_ranges(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported on this flash chip.\n");
			ret = 1;
		}
		goto out_release;
	}

	flashrom_flag_set(fill_flash, FLASHROM_FLAG_FORCE, !!force);
	fill_flash->flags.do_not_diff = do_not_diff;
#if CONFIG_INTERNAL == 1
	flashrom_flag_set(fill_flash, FLASHROM_FLAG_FORCE_BOARDMISMATCH, !!force_boardmismatch);
#endif
	flashrom_flag_set(fill_flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, !dont_verify_it);
	flashrom_flag_set(fill_flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, !dont_verify_all);

	/* FIXME: We should issue an unconditional chip reset here. This can be
	 * done once we have a .reset function in struct flashchip.
	 * Give the chip time to settle.
	 */
	programmer_delay(100000);
	if (read_it)
		ret = do_read(fill_flash, filename);
	else if (extract_it)
		ret = do_extract(fill_flash);
	else if (erase_it)
		ret = do_erase(fill_flash);
	else if (write_it)
		ret = do_write(fill_flash, filename, referencefile);
	else if (verify_it)
		ret = do_verify(fill_flash, filename);

	msg_ginfo("%s\n", ret ? "FAILED" : "SUCCESS");

out_release:
	flashrom_layout_release(layout);
out_shutdown:
	programmer_shutdown();
out:

#if USE_BIG_LOCK == 1
	if (!set_ignore_lock)
		release_big_lock();
#endif
	if (restore_power_management()) {
		msg_gerr("Unable to re-enable power management\n");
		ret |= 1;
	}
	for (i = 0; i < chipcount; i++)
		free(flashes[i].chip);

	layout_cleanup(&include_args);
	free(filename);
	free(fmapfile);
	free(referencefile);
	free(layoutfile);
	free(pparam);
	/* clean up global variables */
	free((char *)chip_to_probe); /* Silence! Freeing is not modifying contents. */
	chip_to_probe = NULL;
#ifndef STANDALONE
	free(logfile);
	ret |= close_logfile();
#endif /* !STANDALONE */
	return ret;
}
