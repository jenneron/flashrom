/*
 * Copyright 2013, Google Inc.
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

#ifndef FLASHMAP_LIB_FDTMAP_H__
#define FLASHMAP_LIB_FDTMAP_H__

#define FDTMAP_SIGNATURE	"__FDTM__"

struct romentry;

/* The header at the start of an fdtmap */
struct fdtmap_hdr {
	char sig[8];		/* Signature (FDTMAP_SIGNATURE) */
	uint32_t size;		/* Size of data region */
	uint32_t crc32;		/* CRC32 of data region */
	/* Data follows immediately */
};

/**
 * fdtmap_add_entries_from_buf()- Add region entries from the FDTMAP
 *
 * @blob: FDT blob containing flashmap node
 * @rom_entries: Place to put the entries we find
 * @max_entries: Maximum number of entries to add
 * @return Number of entries added, or -1 on error
 */
int fdtmap_add_entries_from_buf(const void *blob,
		struct romentry *rom_entries, int max_entries);

/*
 * fdtmap_find - find FDTMAP at offset in an image and copy it to buffer
 *
 * @handle:	opaque pointer to be used by the callback function
 * @read_chunk: callback function which given 'handle', 'offset' and 'size'
 *              will read into the provided memory space 'dest' 'size' bytes
 *              starting from 'offset'. Returns zero on success and non-zero
 *              on failure.
 * @hdr:	pointer to fmap header
 * @offset:	offset of fmap header in image
 * @buf:	unallocated buffer to store fmap struct
 *
 * This function allocates memory which the caller must free.
 *
 * returns 1 if found, 0 if not found, <0 to indicate failure
 */
int fdtmap_find(void *source_handle,
		int (*read_chunk)(void *handle,
				  void *dest,
				  size_t offset,
				  size_t size),
		struct fdtmap_hdr *hdr,
		loff_t offset, uint8_t **buf);

#endif
