/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2005-2008 coresystems GmbH
 * (Written by Stefan Reinauer <stepan@coresystems.de> for coresystems GmbH)
 * Copyright (C) 2011-2013 Stefan Tauner
 * Copyright (C) 2016 secunet Security Networks AG
 * (Written by Nico Huber <nico.huber@secunet.com> for secunet)
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

#ifndef __LAYOUT_H__
#define __LAYOUT_H__ 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Types and macros regarding the maximum flash space size supported by generic code. */
typedef uint32_t chipoff_t; /* Able to store any addressable offset within a supported flash memory. */
typedef uint32_t chipsize_t; /* Able to store the number of bytes of any supported flash memory. */
#define FL_MAX_CHIPOFF_BITS (24)
#define FL_MAX_CHIPOFF ((chipoff_t)(1ULL<<FL_MAX_CHIPOFF_BITS)-1)
#define PRIxCHIPOFF "06"PRIx32
#define PRIuCHIPSIZE PRIu32

#define MAX_ROMLAYOUT	128

struct romentry {
	chipoff_t start;
	chipoff_t end;
	unsigned int included;
	char *name;
	char *file;
};

struct flashrom_layout {
	/* entries store the entries specified in a layout file and associated run-time data */
	struct romentry *entries;
	/* the number of successfully parsed entries */
	size_t num_entries;
};

struct single_layout {
	struct flashrom_layout base;
	struct romentry entry;
};

struct layout_include_args {
	char *name;
	struct layout_include_args *next;
};

struct flashctx;
/**
 * Extract regions to current directory
 *
 * @flash: Information about flash chip to access
 * @return 0 if OK, non-zero on error
 */
int extract_regions(struct flashctx *flash);

struct flashrom_layout *get_global_layout(void);

int find_romentry(struct flashrom_layout *const l, char *name);
int fill_romentry(struct romentry *entry, int n);
int get_num_include_args(const struct flashrom_layout *const l);
int process_include_args(struct flashrom_layout *l, const struct layout_include_args *const args);
int included_regions_overlap(void);
int handle_partial_read(
    struct flashctx *flash,
    uint8_t *buf,
    int (*read) (struct flashctx *flash, uint8_t *buf,
                 unsigned int start, unsigned int len),
    int write_to_file);
    /* RETURN: the number of partitions that have beenpartial read.
    *         ==0 means no partition is specified.
    *         < 0 means writing file error. */

/*
 * In case user specified sections to program (using the -i command line
 * option), prepare new contents such that only the required sections are
 * re-programmed.
 *
 * If no -i command line option was used - do nothing.
 *
 * All areas outside of sections included in -i command line options are set
 * to the same value as old contents (modulo lowest erase block size). This
 * would make sure that those areas remain unchanged.
 *
 * If flashrom was invoked for writing the chip, fill the sections to be
 * written from the user provided image file.
 *
 * If flashrom was invoked for erasing - leave the sections in question
 * untouched, they have been set to flash erase value already.
 */
int build_new_image(const struct flashctx *flash, uint8_t *oldcontents,
		      uint8_t *newcontents, int erase_mode);

#endif /* __LAYOUT_H__ */
