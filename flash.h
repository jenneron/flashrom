/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2000 Ronald G. Minnich <rminnich@gmail.com>
 * Copyright (C) 2005-2009 coresystems GmbH
 * Copyright (C) 2006-2009 Carl-Daniel Hailfinger
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
 *
 */

#ifndef __FLASH_H__
#define __FLASH_H__ 1

#include <stdint.h>
#include <stddef.h>
#include "hwaccess.h"
#ifdef _WIN32
#include <windows.h>
#undef min
#undef max
#endif

/* Are timers broken? */
extern int broken_timer;

struct flashctx; /* forward declare */
#define ERROR_PTR ((void*)-1)

/* Error codes */
#define TIMEOUT_ERROR	-101

/* for verify_it variable in flashrom.c and cli_mfg.c */
enum {
	VERIFY_OFF = 0,
	VERIFY_FULL,
	VERIFY_PARTIAL,
};

typedef unsigned long chipaddr;

int register_shutdown(int (*function) (void *data), void *data);
#define CHIP_RESTORE_CALLBACK	int (*func) (struct flashctx *flash, uint8_t status)

int register_chip_restore(CHIP_RESTORE_CALLBACK, struct flashctx *flash, uint8_t status);
void *programmer_map_flash_region(const char *descr, unsigned long phys_addr,
				  size_t len);
void programmer_unmap_flash_region(void *virt_addr, size_t len);
void programmer_delay(int usecs);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum chipbustype {
	BUS_NONE	= 0,
	BUS_PARALLEL	= 1 << 0,
	BUS_LPC		= 1 << 1,
	BUS_FWH		= 1 << 2,
	BUS_SPI		= 1 << 3,
	BUS_PROG	= 1 << 4,
	BUS_NONSPI	= BUS_PARALLEL | BUS_LPC | BUS_FWH,
};

/* used to select bus which target chip resides */
extern enum chipbustype target_bus;

/*
 * How many different contiguous runs of erase blocks with one size each do
 * we have for a given erase function?
 */
#define NUM_ERASEREGIONS 5

/*
 * How many different erase functions do we have per chip?
 * Atmel AT25FS010 has 6 different functions.
 */
#define NUM_ERASEFUNCTIONS 6

#define FEATURE_REGISTERMAP	(1 << 0)
#define FEATURE_BYTEWRITES	(1 << 1)
#define FEATURE_LONG_RESET	(0 << 4)
#define FEATURE_SHORT_RESET	(1 << 4)
#define FEATURE_EITHER_RESET	FEATURE_LONG_RESET
#define FEATURE_RESET_MASK	(FEATURE_LONG_RESET | FEATURE_SHORT_RESET)
#define FEATURE_ADDR_FULL	(0 << 2)
#define FEATURE_ADDR_MASK	(3 << 2)
#define FEATURE_ADDR_2AA	(1 << 2)
#define FEATURE_ADDR_AAA	(2 << 2)
#define FEATURE_ADDR_SHIFTED	(1 << 5)
#define FEATURE_WRSR_EWSR	(1 << 6)
#define FEATURE_WRSR_WREN	(1 << 7)
#define FEATURE_WRSR_EITHER	(FEATURE_WRSR_EWSR | FEATURE_WRSR_WREN)
#define FEATURE_OTP		(1 << 8)
#define FEATURE_ERASE_TO_ZERO	(1 << 9)
#define FEATURE_UNBOUND_READ	(1 << 10)
#define FEATURE_NO_ERASE	(1 << 11)
#define FEATURE_4BA_SUPPORT	(1 << 12)

struct voltage_range {
	uint16_t min, max;
};

enum test_state {
	OK = 0,
	NT = 1,	/* Not tested */
	BAD,	/* Known to not work */
	DEP,	/* Support depends on configuration (e.g. Intel flash descriptor) */
	NA,	/* Not applicable (e.g. write support on ROM chips) */
};

#define TEST_UNTESTED	(struct tested){ .probe = NT, .read = NT, .erase = NT, .write = NT, .uread = NT }

#define TEST_OK_PROBE	(struct tested){ .probe = OK, .read = NT, .erase = NT, .write = NT, .uread = NT }
#define TEST_OK_PR	(struct tested){ .probe = OK, .read = OK, .erase = NT, .write = NT, .uread = NT }
#define TEST_OK_PRE	(struct tested){ .probe = OK, .read = OK, .erase = OK, .write = NT, .uread = NT }
#define TEST_OK_PRU	(struct tested){ .probe = OK, .read = OK, .erase = NT, .write = NT, .uread = OK }
#define TEST_OK_PREU	(struct tested){ .probe = OK, .read = OK, .erase = OK, .write = NT, .uread = OK }
#define TEST_OK_PREW	(struct tested){ .probe = OK, .read = OK, .erase = OK, .write = OK, .uread = NT }
#define TEST_OK_PREWU	(struct tested){ .probe = OK, .read = OK, .erase = OK, .write = OK, .uread = OK }

#define TEST_BAD_PROBE	(struct tested){ .probe = BAD, .read = NT, .erase = NT, .write = NT, .uread = NT }
#define TEST_BAD_PR	(struct tested){ .probe = BAD, .read = BAD, .erase = NT, .write = NT, .uread = NT }
#define TEST_BAD_PRE	(struct tested){ .probe = BAD, .read = BAD, .erase = BAD, .write = NT, .uread = NT }
#define TEST_BAD_PREW	(struct tested){ .probe = BAD, .read = BAD, .erase = BAD, .write = BAD, .uread = NT }
#define TEST_BAD_PREWU	(struct tested){ .probe = BAD, .read = BAD, .erase = BAD, .write = BAD, .uread = BAD }

struct flashchip {
	const char *vendor;
	const char *name;

	enum chipbustype bustype;

	/*
	 * With 32bit manufacture_id and model_id we can cover IDs up to
	 * (including) the 4th bank of JEDEC JEP106W Standard Manufacturer's
	 * Identification code.
	 */
	uint32_t manufacture_id;
	uint32_t model_id;

	/* Total chip size in kilobytes */
	unsigned int total_size;
	/* Chip page size in bytes */
	unsigned int page_size;
	int feature_bits;

	/* set of function pointers to use in 4-bytes addressing mode */
	struct four_bytes_addr_funcs_set {
		int (*set_4ba) (struct flashctx *flash);
		int (*read_nbyte) (struct flashctx *flash, unsigned int addr, uint8_t *bytes, unsigned int len);
		int (*program_byte) (struct flashctx *flash, unsigned int addr, const uint8_t databyte);
		int (*program_nbyte) (struct flashctx *flash, unsigned int addr, const uint8_t *bytes, unsigned int len);
	} four_bytes_addr_funcs;

	/* Indicate how well flashrom supports different operations of this flash chip. */
	struct tested {
		enum test_state probe;
		enum test_state read;
		enum test_state erase;
		enum test_state write;
		enum test_state uread;
	} tested;

	/*
	 * Group chips that have common command sets. This should ensure that
	 * no chip gets confused by a probing command for a very different class
	 * of chips.
	 */
	enum {
		/* SPI25 is very common. Keep it at zero so we don't have
		   to specify it for each and every chip in the database.*/
		SPI25 = 0,
	} spi_cmd_set;

	int (*probe) (struct flashctx *flash);

	/* Delay after "enter/exit ID mode" commands in microseconds.
	 * NB: negative values have special meanings, see TIMING_* below.
	 */
	signed int probe_timing;

	/*
	 * Erase blocks and associated erase function. Any chip erase function
	 * is stored as chip-sized virtual block together with said function.
	 * The first one that fits will be chosen. There is currently no way to
	 * influence that behaviour. For testing just comment out the other
	 * elements or set the function pointer to NULL.
	 */
	struct block_eraser {
		struct eraseblock {
			unsigned int size; /* Eraseblock size in bytes */
			unsigned int count; /* Number of contiguous blocks with that size */
		} eraseblocks[NUM_ERASEREGIONS];
		/* a block_erase function should try to erase one block of size
		 * 'blocklen' at address 'blockaddr' and return 0 on success. */
		int (*block_erase) (struct flashctx *flash, unsigned int blockaddr, unsigned int blocklen);
	} block_erasers[NUM_ERASEFUNCTIONS];

	int (*printlock) (struct flashctx *flash);
	int (*unlock) (struct flashctx *flash);
	int (*write) (struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len);
	int (*read) (struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len);
	uint8_t (*read_status) (const struct flashctx *flash);
	int (*write_status) (const struct flashctx *flash, int status);
	int (*check_access) (const struct flashctx *flash, unsigned int start, unsigned int len, int read);
	struct voltage_range voltage;
	struct wp *wp;
};

/* struct flashctx must always contain struct flashchip at the beginning. */
struct flashctx {
	struct flashchip *chip;

	chipaddr virtual_memory;
	/* Some flash devices have an additional register space. */
	chipaddr virtual_registers;
};


/* This is the byte value we expect to see in erased regions of the flash */
int flash_erase_value(struct flashctx *flash);

/* This is a byte value that indicates that the region is not erased */
int flash_unerased_value(struct flashctx *flash);

/* Given RDID info, return pointer to entry in flashchips[] */
const struct flashchip *flash_id_to_entry(uint32_t mfg_id, uint32_t model_id);

/* Timing used in probe routines. ZERO is -2 to differentiate between an unset
 * field and zero delay.
 *
 * SPI devices will always have zero delay and ignore this field.
 */
#define TIMING_FIXME	-1
/* this is intentionally same value as fixme */
#define TIMING_IGNORED	-1
#define TIMING_ZERO	-2

extern const struct flashchip flashchips[];
extern const struct flashchip flashchips_hwseq[];

void chip_writeb(const struct flashctx *flash, uint8_t val, chipaddr addr);
void chip_writew(const struct flashctx *flash, uint16_t val, chipaddr addr);
void chip_writel(const struct flashctx *flash, uint32_t val, chipaddr addr);
void chip_writen(const struct flashctx *flash, uint8_t *buf, chipaddr addr, size_t len);
uint8_t chip_readb(const struct flashctx *flash, const chipaddr addr);
uint16_t chip_readw(const struct flashctx *flash, const chipaddr addr);
uint32_t chip_readl(const struct flashctx *flash, const chipaddr addr);
void chip_readn(const struct flashctx *flash, uint8_t *buf, const chipaddr addr, size_t len);

/* print.c */
char *flashbuses_to_text(enum chipbustype bustype);
void print_supported(void);
void print_supported_wiki(void);

/* helpers.c */
uint32_t address_to_bits(uint32_t addr);
int bitcount(unsigned long a);
int min(int a, int b);
int max(int a, int b);
char *strcat_realloc(char *dest, const char *src);
void tolower_string(char *str);

/* flashrom.c */
/*
 * The following enum defines possible write granularities of flash chips. These tend to reflect the properties
 * of the actual hardware not necesserily the write function(s) defined by the respective struct flashchip.
 * The latter might (and should) be more precisely specified, e.g. they might bail out early if their execution
 * would result in undefined chip contents.
 */
enum write_granularity {
	/* We assume 256 byte granularity by default. */
	write_gran_256bytes = 0,/* If less than 256 bytes are written, the unwritten bytes are undefined. */
	write_gran_1bit,	/* Each bit can be cleared individually. */
	write_gran_1byte,	/* A byte can be written once. Further writes to an already written byte cause
				 * its contents to be either undefined or to stay unchanged. */
	write_gran_128bytes,	/* If less than 128 bytes are written, the unwritten bytes are undefined. */
	write_gran_264bytes,	/* If less than 264 bytes are written, the unwritten bytes are undefined. */
	write_gran_512bytes,	/* If less than 512 bytes are written, the unwritten bytes are undefined. */
	write_gran_528bytes,	/* If less than 528 bytes are written, the unwritten bytes are undefined. */
	write_gran_1024bytes,	/* If less than 1024 bytes are written, the unwritten bytes are undefined. */
	write_gran_1056bytes,	/* If less than 1056 bytes are written, the unwritten bytes are undefined. */
	write_gran_1byte_implicit_erase, /* EEPROMs and other chips with implicit erase and 1-byte writes. */
};

extern enum chipbustype buses_supported;
extern enum flashrom_log_level verbose_screen;
extern enum flashrom_log_level verbose_logfile;
extern const char flashrom_version[];
extern char *chip_to_probe;
void map_flash_registers(struct flashctx *flash);
int read_memmapped(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len);
int erase_flash(struct flashctx *flash);
int probe_flash(int startchip, struct flashctx *fill_flash, int force);
int read_flash(struct flashctx *flash, uint8_t *buf,
			unsigned int start, unsigned int len);
int read_flash_to_file(struct flashctx *flash, const char *filename);
char *extract_param(char **haystack, const char *needle, const char *delim);
int verify_range(struct flashctx *flash, uint8_t *cmpbuf, unsigned int start, unsigned int len, const char *message);
void print_version(void);
void print_buildinfo(void);
void print_banner(void);
void list_programmers_linebreak(int startcol, int cols, int paren);
int selfcheck(void);

/*
 *
 * The main processing function of flashrom utility; it is invoked once
 * command line parameters are processed and verified, and the type of the
 * flash chip the programmer operates on has been determined.
 *
 * @flash	  pointer to the flash context matching the chip detected
 *		  during initialization.
 * @force         when set proceed even if the chip is not known to work
 * @filename      pointer to the name of the file to read from or write to
 * @read_it       when true, flash contents are read into 'filename'
 * @write_it      when true, flash is programmed with 'filename' contents
 * @erase_it      when true, flash chip is erased
 * @verify_it	  depending on the value verify the full chip, only changed
 *		  areas, or none
 * @extract_it    extract all known flash chip regions into separate files
 * @diff_file	  when deciding what areas to program, use this file's
 *                contents instead of reading the current chip contents
 * @do_diff	  when true - compare result of the operation with either the
 *		  original chip contents for 'diff_file' contents, is present.
 *		  When false - do not diff, consider the chip erased before
 *		  operation starts.
 *
 * Only one of 'read_it', 'write_it', and 'erase_it' is expected to be set,
 * but this is not enforced.
 *
 * 'do_diff' must be set if 'diff_file' is set. If 'do_diff' is set, but
 * 'diff_file' is not - comparison is done against the pre-operation chip
 * contents.
 */
int doit(struct flashctx *flash, int force, const char *filename, int read_it,
	 int write_it, int erase_it, int verify_it, int extract_it,
	 const char *diff_file, int do_diff);
int read_buf_from_file(unsigned char *buf, unsigned long size, const char *filename);
int write_buf_to_file(unsigned char *buf, unsigned long size, const char *filename);

#define OK 0
#define NT 1    /* Not tested */

/* what to do in case of an error */
enum error_action {
	error_fail,	/* fail immediately */
	error_ignore,	/* non-fatal error; continue */
};

/* Something happened that shouldn't happen, but we can go on. */
#define ERROR_NONFATAL 0x100

/* Something happened that shouldn't happen, we'll abort. */
#define ERROR_FATAL -0xee

/* Operation failed due to access restriction set in programmer or flash chip */
#define ACCESS_DENIED -7
extern enum error_action access_denied_action;

/* convenience function for checking return codes */
extern int ignore_error(int x);

/* cli_output.c */
#ifndef STANDALONE
int open_logfile(const char * const filename);
int close_logfile(void);
void start_logging(void);
#endif
enum flashrom_log_level {
	FLASHROM_MSG_ERROR       = 0,
	FLASHROM_MSG_WARN        = 1,
	FLASHROM_MSG_INFO        = 2,
	FLASHROM_MSG_DEBUG       = 3,
	FLASHROM_MSG_DEBUG2      = 4,
	FLASHROM_MSG_SPEW        = 5,
};
/* Let gcc and clang check for correct printf-style format strings. */
int print(enum flashrom_log_level level, const char *fmt, ...)
#ifdef __MINGW32__
__attribute__((format(gnu_printf, 2, 3)));
#else
__attribute__((format(printf, 2, 3)));
#endif
#define msg_gerr(...)	print(FLASHROM_MSG_ERROR, __VA_ARGS__)	/* general errors */
#define msg_perr(...)	print(FLASHROM_MSG_ERROR, __VA_ARGS__)	/* programmer errors */
#define msg_cerr(...)	print(FLASHROM_MSG_ERROR, __VA_ARGS__)	/* chip errors */
#define msg_gwarn(...)	print(FLASHROM_MSG_WARN, __VA_ARGS__)	/* general warnings */
#define msg_pwarn(...)	print(FLASHROM_MSG_WARN, __VA_ARGS__)	/* programmer warnings */
#define msg_cwarn(...)	print(FLASHROM_MSG_WARN, __VA_ARGS__)	/* chip warnings */
#define msg_ginfo(...)	print(FLASHROM_MSG_INFO, __VA_ARGS__)	/* general info */
#define msg_pinfo(...)	print(FLASHROM_MSG_INFO, __VA_ARGS__)	/* programmer info */
#define msg_cinfo(...)	print(FLASHROM_MSG_INFO, __VA_ARGS__)	/* chip info */
#define msg_gdbg(...)	print(FLASHROM_MSG_DEBUG, __VA_ARGS__)	/* general debug */
#define msg_pdbg(...)	print(FLASHROM_MSG_DEBUG, __VA_ARGS__)	/* programmer debug */
#define msg_cdbg(...)	print(FLASHROM_MSG_DEBUG, __VA_ARGS__)	/* chip debug */
#define msg_gdbg2(...)	print(FLASHROM_MSG_DEBUG2, __VA_ARGS__)	/* general debug2 */
#define msg_pdbg2(...)	print(FLASHROM_MSG_DEBUG2, __VA_ARGS__)	/* programmer debug2 */
#define msg_cdbg2(...)	print(FLASHROM_MSG_DEBUG2, __VA_ARGS__)	/* chip debug2 */
#define msg_gspew(...)	print(FLASHROM_MSG_SPEW, __VA_ARGS__)	/* general debug spew  */
#define msg_pspew(...)	print(FLASHROM_MSG_SPEW, __VA_ARGS__)	/* programmer debug spew  */
#define msg_cspew(...)	print(FLASHROM_MSG_SPEW, __VA_ARGS__)	/* chip debug spew  */

/* spi.c */
struct spi_command {
	unsigned int writecnt;
	unsigned int readcnt;
	const unsigned char *writearr;
	unsigned char *readarr;
};
#define NULL_SPI_CMD { 0, 0, NULL, NULL, }
int spi_send_command(const struct flashctx *flash, unsigned int writecnt, unsigned int readcnt,
		const unsigned char *writearr, unsigned char *readarr);
int spi_send_multicommand(const struct flashctx *flash, struct spi_command *cmds);
uint32_t spi_get_valid_read_addr(struct flashctx *flash);

#define NUM_VOLTAGE_RANGES	16
extern struct voltage_range voltage_ranges[];
/* returns number of unique voltage ranges, or <0 to indicate failure */
extern int flash_supported_voltage_ranges(enum chipbustype bus);

#endif				/* !__FLASH_H__ */
