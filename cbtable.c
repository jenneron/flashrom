/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2002 Steven James <pyro@linuxlabs.com>
 * Copyright (C) 2002 Linux Networx
 * (Written by Eric Biederman <ebiederman@lnxi.com> for Linux Networx)
 * Copyright (C) 2006-2009 coresystems GmbH
 * (Written by Stefan Reinauer <stepan@coresystems.de> for coresystems GmbH)
 * Copyright (C) 2010 Carl-Daniel Hailfinger
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include "flash.h"
#include "programmer.h"
#include "coreboot_tables.h"

static char *cb_vendor = NULL, *cb_model = NULL;
int partvendor_from_cbtable = 0;

/* Parse the [<vendor>:]<board> string specified by the user as part of
 * -p internal:mainboard=[<vendor>:]<board> and set cb_vendor and cb_model
 * to the extracted values.
 * Note: strtok modifies the original string, so we work on a copy and allocate
 * memory for cb_vendor and cb_model with strdup.
 */
void lb_vendor_dev_from_string(const char *boardstring)
{
	/* strtok may modify the original string. */
	char *tempstr = strdup(boardstring);
	char *tempstr2 = NULL;
	strtok(tempstr, ":");
	tempstr2 = strtok(NULL, ":");
	if (tempstr2) {
		cb_vendor = strdup(tempstr);
		cb_model = strdup(tempstr2);
	} else {
		cb_vendor = NULL;
		cb_model = strdup(tempstr);
	}
	free(tempstr);
}

static unsigned long compute_checksum(void *addr, unsigned long length)
{
	uint8_t *ptr;
	volatile union {
		uint8_t byte[2];
		uint16_t word;
	} chksum;
	unsigned long sum;
	unsigned long i;

	/* In the most straight forward way possible,
	 * compute an ip style checksum.
	 */
	sum = 0;
	ptr = addr;
	for (i = 0; i < length; i++) {
		unsigned long value;
		value = ptr[i];
		if (i & 1) {
			value <<= 8;
		}
		/* Add the new value */
		sum += value;
		/* Wrap around the carry */
		if (sum > 0xFFFF) {
			sum = (sum + (sum >> 16)) & 0xFFFF;
		}
	}
	chksum.byte[0] = sum & 0xff;
	chksum.byte[1] = (sum >> 8) & 0xff;

	return (~chksum.word) & 0xFFFF;
}

#define for_each_lbrec(head, rec) \
	for(rec = (struct lb_record *)(((char *)head) + sizeof(*head)); \
		(((char *)rec) < (((char *)head) + sizeof(*head) + head->table_bytes))  && \
		(rec->size >= 1) && \
		((((char *)rec) + rec->size) <= (((char *)head) + sizeof(*head) + head->table_bytes)); \
		rec = (struct lb_record *)(((char *)rec) + rec->size))

static unsigned int count_lb_records(struct lb_header *head)
{
	struct lb_record *rec;
	unsigned int count;

	count = 0;
	for_each_lbrec(head, rec) {
		count++;
	}

	return count;
}

static int lb_header_valid(struct lb_header *head, unsigned long addr)
{
	if (memcmp(head->signature, "LBIO", 4) != 0)
		return 0;
	msg_pdbg("Found candidate at: %08lx-%08lx\n",
		     addr, addr + sizeof(*head) + head->table_bytes);
	if (head->header_bytes != sizeof(*head)) {
		msg_perr("Header bytes of %d are incorrect.\n",
			head->header_bytes);
		return 0;
	}
	if (compute_checksum((uint8_t *) head, sizeof(*head)) != 0) {
		msg_perr("Bad header checksum.\n");
		return 0;
	}

	return 1;
}

static int lb_table_valid(struct lb_header *head, struct lb_record *recs)
{
	if (compute_checksum(recs, head->table_bytes)
	    != head->table_checksum) {
		msg_perr("Bad table checksum: %04x.\n",
			head->table_checksum);
		return 0;
	}
	if (count_lb_records(head) != head->table_entries) {
		msg_perr("Bad record count: %d.\n",
			head->table_entries);
		return 0;
	}

	return 1;
}

static struct lb_header *find_lb_table(void *base, unsigned long start,
				       unsigned long end)
{
	unsigned long addr;

	/* For now be stupid.... */
	for (addr = start; addr < end; addr += 16) {
		struct lb_header *head =
		    (struct lb_header *)(((char *)base) + addr);
		struct lb_record *recs =
		    (struct lb_record *)(((char *)base) + addr + sizeof(*head));
		if (!lb_header_valid(head, addr))
			continue;
		if (!lb_table_valid(head, recs))
			continue;
		msg_pdbg("Found coreboot table at 0x%08lx.\n", addr);
		return head;

	}

	return NULL;
}

static struct lb_header *find_lb_table_remap(unsigned long start_addr,
					     uint8_t **table_area)
{
	size_t offset;
	unsigned long addr, end;
	size_t mapping_size;
	void *base;

	mapping_size = getpagesize();
	offset = start_addr % getpagesize();
	start_addr -= offset;

	base = physmap_ro("high tables", start_addr, mapping_size);
	if (ERROR_PTR == base) {
		msg_perr("Failed getting access to coreboot high tables.\n");
		return NULL;
	}

	for (addr = offset, end = getpagesize(); addr < end; addr += 16) {
		struct lb_record *recs;
		struct lb_header *head;

		/* No more headers to check. */
		if (end - addr < sizeof(*head))
			return NULL;

		head = (struct lb_header *)(((char *)base) + addr);

		if (!lb_header_valid(head, addr))
			continue;

		if (mapping_size - addr < head->table_bytes + sizeof(*head)) {
			size_t prev_mapping_size = mapping_size;
			mapping_size = head->table_bytes + sizeof(*head);
			mapping_size += addr;
			mapping_size += getpagesize() -
				(mapping_size % getpagesize());
			physunmap(base, prev_mapping_size);
			base = physmap_ro("high tables", start_addr,
						mapping_size);
			if (ERROR_PTR == base) {
				msg_perr("Failed getting access to coreboot high tables.\n");
				return NULL;
			}
		}

		head = (struct lb_header *)(((char *)base) + addr);
		recs =
		    (struct lb_record *)(((char *)base) + addr + sizeof(*head));
		if (!lb_table_valid(head, recs))
			continue;
		msg_pdbg("Found coreboot table at 0x%08lx.\n", addr);
		*table_area = base;
		return head;
	}

	physunmap(base, mapping_size);
	return NULL;
}

static void find_mainboard(struct lb_record *ptr, unsigned long addr)
{
	struct lb_mainboard *rec;
	int max_size;
	char vendor[256], part[256];

	rec = (struct lb_mainboard *)ptr;
	max_size = rec->size - sizeof(*rec);
	msg_pdbg("Vendor ID: %.*s, part ID: %.*s\n",
	         max_size - rec->vendor_idx,
	         rec->strings + rec->vendor_idx,
	         max_size - rec->part_number_idx,
	         rec->strings + rec->part_number_idx);
	snprintf(vendor, 255, "%.*s", max_size - rec->vendor_idx, rec->strings + rec->vendor_idx);
	snprintf(part, 255, "%.*s", max_size - rec->part_number_idx, rec->strings + rec->part_number_idx);

	if (cb_model) {
		msg_pdbg("Overwritten by command line, vendor ID: %s, part ID: %s.\n", cb_vendor, cb_model);
	} else {
		partvendor_from_cbtable = 1;
		cb_model = strdup(part);
		cb_vendor = strdup(vendor);
	}
}

static struct lb_record *next_record(struct lb_record *rec)
{
	return (struct lb_record *)(((char *)rec) + rec->size);
}

static void search_lb_records(struct lb_record *rec, struct lb_record *last, unsigned long addr)
{
	struct lb_record *next;
	int count;
	count = 0;

	for (next = next_record(rec); (rec < last) && (next <= last);
	     rec = next, addr += rec->size) {
		next = next_record(rec);
		count++;
		if (rec->tag == LB_TAG_MAINBOARD) {
			find_mainboard(rec, addr);
			break;
		}
	}
}

#define BYTES_TO_MAP (1024*1024)
/* returns 0 if the table was parsed successfully and cb_vendor/cb_model have been set. */
int cb_parse_table(const char **vendor, const char **model)
{
	uint8_t *table_area;
	unsigned long addr, start;
	struct lb_header *lb_table;
	struct lb_record *rec, *last;

#if defined(__MACH__) && defined(__APPLE__)
	/* This is a hack. DirectHW fails to map physical address 0x00000000.
	 * Why?
	 */
	start = 0x400;
#else
	start = 0x0;
#endif
	table_area = physmap_ro_unaligned("low megabyte", start, BYTES_TO_MAP - start);
	if (ERROR_PTR == table_area) {
		msg_perr("Failed getting access to coreboot low tables.\n");
		return -1;
	}

	lb_table = find_lb_table(table_area, 0x00000, 0x1000);
	if (!lb_table)
		lb_table = find_lb_table(table_area, 0xf0000 - start, BYTES_TO_MAP - start);
	if (lb_table) {
		struct lb_forward *forward = (struct lb_forward *)
			(((char *)lb_table) + lb_table->header_bytes);
		if (forward->tag == LB_TAG_FORWARD) {
			start = forward->forward;
			physunmap_unaligned(table_area, BYTES_TO_MAP);
			lb_table = find_lb_table_remap(start, &table_area);
		}
	}

	if (!lb_table) {
		msg_pdbg("No coreboot table found.\n");
		return -1;
	}

	addr = ((char *)lb_table) - ((char *)table_area) + start;
	msg_pinfo("coreboot table found at 0x%lx.\n",
		(unsigned long)lb_table - (unsigned long)table_area + start);
	rec = (struct lb_record *)(((char *)lb_table) + lb_table->header_bytes);
	last = (struct lb_record *)(((char *)rec) + lb_table->table_bytes);
	msg_pdbg("coreboot header(%d) checksum: %04x table(%d) checksum: %04x entries: %d\n",
	     lb_table->header_bytes, lb_table->header_checksum,
	     lb_table->table_bytes, lb_table->table_checksum,
	     lb_table->table_entries);
	search_lb_records(rec, last, addr + lb_table->header_bytes);
	*vendor = cb_vendor;
	*model = cb_model;
	return 0;
}
