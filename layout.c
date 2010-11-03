/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2005-2008 coresystems GmbH
 * (Written by Stefan Reinauer <stepan@coresystems.de> for coresystems GmbH)
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "flash.h"
#include "programmer.h"

#if CONFIG_INTERNAL == 1
char *mainboard_vendor = NULL;
char *mainboard_part = NULL;
#endif
static int romimages = 0;

#define MAX_ROMLAYOUT	32

typedef struct {
	unsigned int start;
	unsigned int end;
	unsigned int included;
	char name[256];
} romlayout_t;

static romlayout_t rom_entries[MAX_ROMLAYOUT];

#if CONFIG_INTERNAL == 1 /* FIXME: Move the whole block to cbtable.c? */
static char *def_name = "DEFAULT";

int show_id(uint8_t *bios, int size, int force)
{
	unsigned int *walk;
	unsigned int mb_part_offset, mb_vendor_offset;
	char *mb_part, *mb_vendor;

	mainboard_vendor = def_name;
	mainboard_part = def_name;

	walk = (unsigned int *)(bios + size - 0x10);
	walk--;

	if ((*walk) == 0 || ((*walk) & 0x3ff) != 0) {
		/* We might have an NVIDIA chipset BIOS which stores the ID
		 * information at a different location.
		 */
		walk = (unsigned int *)(bios + size - 0x80);
		walk--;
	}

	/*
	 * Check if coreboot last image size is 0 or not a multiple of 1k or
	 * bigger than the chip or if the pointers to vendor ID or mainboard ID
	 * are outside the image of if the start of ID strings are nonsensical
	 * (nonprintable and not \0).
	 */
	mb_part_offset = *(walk - 1);
	mb_vendor_offset = *(walk - 2);
	if ((*walk) == 0 || ((*walk) & 0x3ff) != 0 || (*walk) > size ||
	    mb_part_offset > size || mb_vendor_offset > size) {
		msg_pdbg("Flash image seems to be a legacy BIOS. Disabling checks.\n");
		return 0;
	}

	mb_part = (char *)(bios + size - mb_part_offset);
	mb_vendor = (char *)(bios + size - mb_vendor_offset);
	if (!isprint((unsigned char)*mb_part) ||
	    !isprint((unsigned char)*mb_vendor)) {
		msg_pdbg("Flash image seems to have garbage in the ID location."
		       " Disabling checks.\n");
		return 0;
	}

	msg_pdbg("coreboot last image size "
		     "(not ROM size) is %d bytes.\n", *walk);

	mainboard_part = strdup(mb_part);
	mainboard_vendor = strdup(mb_vendor);
	msg_pdbg("Manufacturer: %s\n", mainboard_vendor);
	msg_pdbg("Mainboard ID: %s\n", mainboard_part);

	/*
	 * If lb_vendor is not set, the coreboot table was
	 * not found. Nor was -m VENDOR:PART specified.
	 */
	if (!lb_vendor || !lb_part) {
		msg_pdbg("Note: If the following flash access fails, "
		       "try -m <vendor>:<mainboard>.\n");
		return 0;
	}

	/* These comparisons are case insensitive to make things
	 * a little less user^Werror prone. 
	 */
	if (!strcasecmp(mainboard_vendor, lb_vendor) &&
	    !strcasecmp(mainboard_part, lb_part)) {
		msg_pdbg("This firmware image matches this mainboard.\n");
	} else {
		if (force_boardmismatch) {
			msg_pinfo("WARNING: This firmware image does not "
			       "seem to fit to this machine - forcing it.\n");
		} else {
			msg_pinfo("ERROR: Your firmware image (%s:%s) does not "
			       "appear to\n       be correct for the detected "
			       "mainboard (%s:%s)\n\nOverride with -p internal:"
			       "boardmismatch=force if you are absolutely sure "
			       "that\nyou are using a correct "
			       "image for this mainboard or override\nthe detected "
			       "values with --mainboard <vendor>:<mainboard>.\n\n",
			       mainboard_vendor, mainboard_part, lb_vendor,
			       lb_part);
			exit(1);
		}
	}

	return 0;
}
#endif

int read_romlayout(char *name)
{
	FILE *romlayout;
	char tempstr[256];
	int i;

	romlayout = fopen(name, "r");

	if (!romlayout) {
		msg_gerr("ERROR: Could not open ROM layout (%s).\n",
			name);
		return -1;
	}

	while (!feof(romlayout)) {
		char *tstr1, *tstr2;
		if (2 != fscanf(romlayout, "%s %s\n", tempstr, rom_entries[romimages].name))
			continue;
#if 0
		// fscanf does not like arbitrary comments like that :( later
		if (tempstr[0] == '#') {
			continue;
		}
#endif
		tstr1 = strtok(tempstr, ":");
		tstr2 = strtok(NULL, ":");
		if (!tstr1 || !tstr2) {
			msg_gerr("Error parsing layout file.\n");
			fclose(romlayout);
			return 1;
		}
		rom_entries[romimages].start = strtol(tstr1, (char **)NULL, 16);
		rom_entries[romimages].end = strtol(tstr2, (char **)NULL, 16);
		rom_entries[romimages].included = 0;
		romimages++;
	}

	for (i = 0; i < romimages; i++) {
		msg_gdbg("romlayout %08x - %08x named %s\n",
			     rom_entries[i].start,
			     rom_entries[i].end, rom_entries[i].name);
	}

	fclose(romlayout);

	return 0;
}

int find_romentry(char *name)
{
	int i;

	if (!romimages)
		return -1;

	msg_gdbg("Looking for \"%s\"... ", name);

	for (i = 0; i < romimages; i++) {
		if (!strcmp(rom_entries[i].name, name)) {
			rom_entries[i].included = 1;
			msg_gdbg("found.\n");
			return i;
		}
	}
	msg_gdbg("not found.\n");	// Not found. Error.

	return -1;
}

int handle_romentries(uint8_t *buffer, struct flashchip *flash)
{
	int i;

	// This function does not save flash write cycles.
	// 
	// Also it does not cope with overlapping rom layout
	// sections. 
	// example:
	// 00000000:00008fff gfxrom
	// 00009000:0003ffff normal
	// 00040000:0007ffff fallback
	// 00000000:0007ffff all
	//
	// If you'd specify -i all the included flag of all other
	// sections is still 0, so no changes will be made to the
	// flash. Same thing if you specify -i normal -i all only 
	// normal will be updated and the rest will be kept.

	for (i = 0; i < romimages; i++) {

		if (rom_entries[i].included)
			continue;

		flash->read(flash, buffer + rom_entries[i].start,
			    rom_entries[i].start,
			    rom_entries[i].end - rom_entries[i].start + 1);
	}

	return 0;
}

/* Given an addr, this function returns if the addr falls in the regions
 * those user specifies with -i option (partition).
 *
 * Returns: 1 if this addr falls in -i regions.
 *          0 if not in regions.
 */
int in_valid_romentry(const chipaddr addr) {
	int i;

	if (romimages) {
		for (i = 0; i < romimages; ++i) {
			if (!rom_entries[i].included)
				continue;

			if ((addr >= rom_entries[i].start) &&
			    (addr <= rom_entries[i].end)) {
				return 1;
			}
		}
		return 0;
	} else {
		return 1;  /* always TRUE if no layout is specified */
	}

	return 0;
}

/** Given a callback function, only *included* entries apply the callback.
 *  @buffer - the start of buffer
 *  @flash - point to flash info
 *  @do_action - callback function that handles included entry.
 */
int do_romentries(uint8_t *buffer, struct flashchip *flash,
                  int (*do_action)(struct flashchip *flash,
                                   uint8_t *buf,
                                   const chipaddr addr, size_t len))
{
	int i;
	int len;
	if (!do_action) return 1;

	if (romimages) {
		for (i = 0; i < romimages; i++) {
			if (!rom_entries[i].included)
				continue;

			len = rom_entries[i].end - rom_entries[i].start + 1;
			msg_gdbg("Doing %p(%p) 0x%08x (len=0x%08x)\n",
			         do_action, buffer,
			         rom_entries[i].start, len);
			if (do_action(flash, buffer,
			              rom_entries[i].start, len))
				return 1;
		}
	} else {
		/* no layout table specified, apply to whole flash. */
		len = flash->total_size * 1024;
		if (do_action(flash, buffer, 0, len))
			return 1;
	}

	return 0;
}
