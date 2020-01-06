#!/bin/sh
#
# Copyright (C) 2010 Google Inc.
# Written by David Hendricks for Google Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

# Assume we can set a write protect range for 0-64KB
# This assumes the output from wp-status is:
# "some text ... start=0x00000000, len=0x00010000"
# where the start and length are key=value pairs

# FIXME: Should we intentionally disable write protection for this? The
# status register will not be changeable if hardware WP is in effect.
# Then again, if hardware WP is on, then we won't be able to disable WP
# either...
#
# FIXME #2: As per Carl-Daniel's comment, this test does not adequately
# check that encoding is correct. It only checks if the output matches
# the requested range.
#

. "$(pwd)/common.sh"

logfile="${0}.log"

# Try to write protect the uppermost block
new_wp_range_start=$((($(./flashrom ${FLASHROM_PARAM} --flash-size 2>/dev/null) - 0x010000)))
new_wp_range_len=0x010000

# Back-up old settings
tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null)
old_start=${tmp##*start=}
old_start=`printf "%s" "${old_start%%,*}"`

old_len=${tmp##*len=}
old_len=`printf "%s" "${old_len%%,*}"`
echo "old start: ${old_start}, old length: ${old_len}" >> ${logfile}

# If the old write protection settings are the same as the new ones, we need
# to choose new values. If this is the case, we'll drop the lower bound of the
# range by 1 block and extend the range 64K block.
if [ $((${old_start} == ${new_wp_range_start})) -ne 1 ] && [ $((${old_len} == ${new_wp_range_len})) -ne 1 ] ; then
	new_wp_range_start=$((${new_wp_range_start} - 0x10000))
	new_wp_range_len=$((${new_wp_range_len} + 0x10000))
fi

# Try to set new range values
echo "attempting to set write protect range: start=${new_wp_range_start} ${new_wp_range_len}" >> ${logfile}
do_test_flashrom --wp-range ${new_wp_range_start} ${new_wp_range_len}
if [ ${?} -ne ${EXIT_SUCCESS} ]; then
	echo -n "failed to set write protect range" >> ${logfile}
	return ${EXIT_FAILURE}
fi

tmp=$(./flashrom ${FLASHROM_PARAM} --wp-status 2>/dev/null)
new_start=${tmp##*start=}
new_start=`printf "%s" "${new_start%%,*}"`

new_len=${tmp##*len=}
new_len=`printf "%s" "${new_len%%,*}"`

if [ $((${new_start} == ${new_wp_range_start})) -ne 1 ]; then return ${EXIT_FAILURE} ; fi
if [ $((${new_len} == ${new_wp_range_len})) -ne 1 ]; then return ${EXIT_FAILURE} ; fi

# restore the old settings
do_test_flashrom --wp-range ${old_start} ${old_len}
if [ ${?} -ne ${EXIT_SUCCESS} ]; then
	echo "failed to restore old settings" >> ${logfile}
	return ${EXIT_FAILURE}
fi

echo "$0: passed" >> ${logfile}
return ${EXIT_SUCCESS}
