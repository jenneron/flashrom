/*
 * Copyright 2015, Google Inc.
 * Copyright 2018-present, Facebook Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 */

#ifndef __FMAP_H__
#define __FMAP_H__ 1

#include <inttypes.h>
#include <stdbool.h>

struct flashctx;

#define FMAP_SIGNATURE		"__FMAP__"
#define FMAP_VER_MAJOR		1	/* this header's FMAP minor version */
#define FMAP_VER_MINOR		1	/* this header's FMAP minor version */
#define FMAP_STRLEN		32	/* maximum length for strings, */
					/* including null-terminator */

struct fmap_area {
	uint32_t offset;		/* offset relative to base */
	uint32_t size;			/* size in bytes */
	uint8_t  name[FMAP_STRLEN];	/* descriptive name */
	uint16_t flags;			/* flags for this area */
}  __attribute__((packed));
/* Mapping of volatile and static regions in firmware binary */
struct fmap {
	uint64_t signature;		/* "__FMAP__" (0x5F5F50414D465F5F) */
	uint8_t  ver_major;		/* major version */
	uint8_t  ver_minor;		/* minor version */
	uint64_t base;			/* address of the firmware binary */
	uint32_t size;			/* size of firmware binary in bytes */
	uint8_t  name[FMAP_STRLEN];	/* name of this firmware binary */
	uint16_t nareas;		/* number of areas described by
					   fmap_areas[] below */
	struct fmap_area areas[];
} __attribute__((packed));

struct search_info;

/*
 * fmap_find - find FMAP signature at offset in an image and copy it to buffer
 *
 * @handle:	opaque pointer to be used by the callback function
 * @read_chunk: callback function which given 'handle', 'offset' and 'size'
 *              will read into the provided memory space 'dest' 'size' bytes
 *              starting from 'offset'. Returns zero on success and non-zero
 *              on failure.
 * @fmap:	pointer to fmap header
 * @offset:	offset of fmap header in image
 * @buf:	unallocated buffer to store fmap struct
 *
 * This function allocates memory which the caller must free. It does no error
 * checking. The caller is responsible for verifying that the contents are
 * sane.
 *
 * returns 1 if found, 0 if not found, <0 to indicate failure
 */
int fmap_find(void *source_handle,
	      int (*read_chunk)(void *handle,
				void *dest,
				size_t offset,
				size_t size),
	      struct fmap *fmap,
	      loff_t offset,
	      uint8_t **buf);

/* Like fmap_find, but give a memory location to search FMAP. */
struct fmap *fmap_find_in_memory(uint8_t *image, int size);


#endif	/* __FMAP_H__*/
