/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000-2002 Alan Cox <alan@redhat.com>
 * Copyright (C) 2002-2010 Jean Delvare <khali@linux-fr.org>
 * Copyright (C) 2009,2010 Michael Karcher
 * Copyright (C) 2011-2013 Stefan Tauner
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

/* strnlen is in POSIX but was a GNU extension up to glibc 2.10 */
#if (__GLIBC__ == 2 && __GLIBC_MINOR__ < 10) || __GLIBC__ < 2
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"
#include "flash.h"
#include "programmer.h"

int has_dmi_support = 0;

#if STANDALONE

/* Stub to indicate missing DMI functionality.
 * has_dmi_support is 0 by default, so nothing to do here.
 * Because dmidecode is not available on all systems, the goal is to implement
 * the DMI subset we need directly in this file.
 */
void dmi_init(void)
{
}

int dmi_match(const char *pattern)
{
	return 0;
}

#else /* STANDALONE */

/* Strings longer than 4096 in DMI are just insane. */
#define DMI_MAX_ANSWER_LEN 4096

static struct {
	const char *const keyword;
	const uint8_t type;
	const uint8_t offset;
	char *value;
} dmi_strings[] = {
	{ "system-manufacturer", 1, 0x04, NULL },
	{ "system-product-name", 1, 0x05, NULL },
	{ "system-version", 1, 0x06, NULL },
	{ "baseboard-manufacturer", 2, 0x04, NULL },
	{ "baseboard-product-name", 2, 0x05, NULL },
	{ "baseboard-version", 2, 0x06, NULL },
};

/* This list is used to identify supposed laptops. The is_laptop field has the
 * following meaning:
 *	- 0: in all likelihood not a laptop
 *	- 1: in all likelihood a laptop
 *	- 2: chassis-type is not specific enough
 * A full list of chassis types can be found in the System Management BIOS
 * (SMBIOS) Reference Specification 2.7.0 section 7.4.1 "Chassis Types" at
 * http://www.dmtf.org/sites/default/files/standards/documents/DSP0134_2.7.0.pdf
 * The types below are the most common ones.
 */
static const struct {
	uint8_t type;
	uint8_t is_laptop;
	const char *name;
} dmi_chassis_types[] = {
	{0x01, 2, "Other"},
	{0x02, 2, "Unknown"},
	{0x03, 0, "Desktop"},
	{0x04, 0, "Low Profile Desktop"},
	{0x06, 0, "Mini Tower"},
	{0x07, 0, "Tower"},
	{0x08, 1, "Portable"},
	{0x09, 1, "Laptop"},
	{0x0a, 1, "Notebook"},
	{0x0b, 1, "Hand Held"},
	{0x0e, 1, "Sub Notebook"},
	{0x11, 0, "Main Server Chassis"},
	{0x17, 0, "Rack Mount Chassis"},
	{0x18, 0, "Sealed-case PC"}, /* used by Supermicro (X8SIE) */
	{0x19, 0, "Multi-system"}, /* used by Supermicro (X7DWT) */
};

#define DMI_COMMAND_LEN_MAX 300
#if IS_WINDOWS
static const char *dmidecode_command = "dmidecode.exe 2>NUL";
#else
static const char *dmidecode_command = "dmidecode 2>/dev/null";
#endif

static char *get_dmi_string(const char *string_name)
{
	FILE *dmidecode_pipe;
	char *result;
	char answerbuf[DMI_MAX_ANSWER_LEN];
	char commandline[DMI_COMMAND_LEN_MAX];

	snprintf(commandline, sizeof(commandline),
		 "%s -s %s", dmidecode_command, string_name);
	dmidecode_pipe = popen(commandline, "r");
	if (!dmidecode_pipe) {
		msg_perr("Opening DMI pipe failed!\n");
		return NULL;
	}

	/* Kill lines starting with '#', as recent dmidecode versions
	 * have the quirk to emit a "# SMBIOS implementations newer..."
	 * message even on "-s" if the SMBIOS declares a
	 * newer-than-supported version number, while it *should* only print
	 * the requested string.
	 */
	do {
		if (!fgets(answerbuf, DMI_MAX_ANSWER_LEN, dmidecode_pipe)) {
			if (ferror(dmidecode_pipe)) {
				msg_perr("DMI pipe read error\n");
				pclose(dmidecode_pipe);
				return NULL;
			}
			answerbuf[0] = 0;	/* Hit EOF */
		}
	} while (answerbuf[0] == '#');

	/* Discard all output exceeding DMI_MAX_ANSWER_LEN to prevent deadlock on pclose. */
	while (!feof(dmidecode_pipe))
		getc(dmidecode_pipe);
	if (pclose(dmidecode_pipe) != 0) {
		msg_pwarn("dmidecode execution unsuccessful - continuing without DMI info\n");
		return NULL;
	}

	/* Chomp trailing newline. */
	if (answerbuf[0] != 0 && answerbuf[strlen(answerbuf) - 1] == '\n')
		answerbuf[strlen(answerbuf) - 1] = 0;

	result = strdup(answerbuf);
	if (result == NULL)
		msg_pwarn("Warning: Out of memory - DMI support fails");

	return result;
}

static int dmi_fill(void)
{
	unsigned int i;
	char *chassis_type;

	msg_pdbg("Using External DMI decoder.\n");
	for (i = 0; i < ARRAY_SIZE(dmi_strings); i++) {
		dmi_strings[i].value = get_dmi_string(dmi_strings[i].keyword);
		if (dmi_strings[i].value == NULL)
			return 1;
	}

	chassis_type = get_dmi_string("chassis-type");
	if (chassis_type == NULL)
		return 0; /* chassis-type handling is optional anyway */

	msg_pdbg("DMI string chassis-type: \"%s\"\n", chassis_type);
	is_laptop = 2;
	for (i = 0; i < ARRAY_SIZE(dmi_chassis_types); i++) {
		if (strcasecmp(chassis_type, dmi_chassis_types[i].name) == 0) {
			is_laptop = dmi_chassis_types[i].is_laptop;
			break;
		}
	}
	free(chassis_type);
	return 0;
}

static int dmi_shutdown(void *data)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(dmi_strings); i++) {
		free(dmi_strings[i].value);
		dmi_strings[i].value = NULL;
	}
	return 0;
}

void dmi_init(void)
{
	/* Register shutdown function before we allocate anything. */
	if (register_shutdown(dmi_shutdown, NULL)) {
		msg_pwarn("Warning: Could not register DMI shutdown function - continuing without DMI info.\n");
		return;
	}

	/* dmi_fill fills the dmi_strings array, and if possible sets the global is_laptop variable. */
	if (dmi_fill() != 0)
		return;

	switch (is_laptop) {
	case 1:
		msg_pdbg("Laptop detected via DMI.\n");
		break;
	case 2:
		msg_pdbg("DMI chassis-type is not specific enough.\n");
		break;
	}

	has_dmi_support = 1;
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(dmi_strings); i++) {
		msg_pdbg("DMI string %s: \"%s\"\n", dmi_strings[i].keyword,
			 (dmi_strings[i].value == NULL) ? "" : dmi_strings[i].value);
	}
}

/**
 * Does an substring/prefix/postfix/whole-string match.
 *
 * The pattern is matched as-is. The only metacharacters supported are '^'
 * at the beginning and '$' at the end. So you can look for "^prefix",
 * "suffix$", "substring" or "^complete string$".
 *
 * @param value The non-NULL string to check.
 * @param pattern The non-NULL pattern.
 * @return Nonzero if pattern matches.
 */
static int dmi_compare(const char *value, const char *pattern)
{
	int anchored = 0;
	int patternlen;

	msg_pspew("matching %s against %s\n", value, pattern);
	/* The empty string is part of all strings! */
	if (pattern[0] == 0)
		return 1;

	if (pattern[0] == '^') {
		anchored = 1;
		pattern++;
	}

	patternlen = strlen(pattern);
	if (pattern[patternlen - 1] == '$') {
		int valuelen = strlen(value);
		patternlen--;
		if (patternlen > valuelen)
			return 0;

		/* full string match: require same length */
		if (anchored && (valuelen != patternlen))
			return 0;

		/* start character to make ends match */
		value += valuelen - patternlen;
		anchored = 1;
	}

	if (anchored)
		return strncmp(value, pattern, patternlen) == 0;
	else
		return strstr(value, pattern) != NULL;
}

int dmi_match(const char *pattern)
{
	unsigned int i;

	if (!has_dmi_support)
		return 0;

	for (i = 0; i < ARRAY_SIZE(dmi_strings); i++) {
		if (dmi_strings[i].value == NULL)
			continue;

		if (dmi_compare(dmi_strings[i].value, pattern))
			return 1;
	}

	return 0;
}

#endif /* STANDALONE */
