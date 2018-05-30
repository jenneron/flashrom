/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef __FLASHROM_ACTION_DESCRIPTOR_H
#define __FLASHROM_ACTION_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Structure containing information about the processing flashrom is supposed
 * to perform on the current run.
 *
 * The variable size 'processing_units' array contains description of
 * processing to the flash block granularity.
 */
struct action_descriptor {
	void *oldcontents;
	void *newcontents;
	struct processing_unit {
		size_t block_size;  /* Block size granularity of this unit. */
		size_t offset;	    /* Offset of the first block. */
		size_t num_blocks;  /* Number of consecutive blocks. Value of
				     * zero indicates the last entry in the
				     * processing_units array. */
		int block_eraser_index;  /* Index into 'block_erasers'. */
		int block_region_index;  /* Index into 'eraseblocks'. */
	} processing_units[0];
};

/* Forward reference for the flash descriptor structure defined in flash.h. */
struct flashctx;

/*
 * Function to create an action descriptor based on the 'before' and 'after'
 * flash contents.
 */
struct action_descriptor *prepare_action_descriptor(struct flashctx *flash,
						    void *oldcontents,
						    void *newcontents,
						    int do_diff);

/*
 * A function to test action descriptor implementation, returns number of
 * failures.
 */
int test_action_descriptor(void);

#endif  /* ! __FLASHROM_ACTION_DESCRIPTOR_H */
