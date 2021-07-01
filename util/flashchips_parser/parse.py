#!/usr/bin/python3
"""Parses and manipulates flashchips.c

This program reads each entry in the flashchips structure and can manipulate the
file in the following ways:
 - compare entries within a file to assist with identifying duplicates
 - rewrite a file in original format, or sorted alphabetically
 - compare entries between upstream and chromium versions of flashchips.c

This code is intended only to support the rejoining of the Chromium fork with
upstream. It is not intended to be a generally useful tool or to have a long
lifetime. It is not easily maintainable without significant rework.

For these reasons, this file should be deleted as soon as it is no longer being
used. It should definitely be deleted at the point where Chromium transitions to
using upstream and we can delete the entire fork.

 Example usage. Shows chip names from upstream:
    python3 parse.py names --use=upstream
"""

import argparse
from pathlib import Path
import re
import sys
import csv

TMP_DIR = Path.home() / 'tmp'
DIRS = {
    'chromiumos': Path.home() / 'chromiumos/src/third_party/flashrom/',
    'tmp': TMP_DIR,
    'upstream': Path.home() / 'coreboot/flashrom'
}
FLASHCHIPS_C = 'flashchips.c'
NEW_FLASHCHIPS_C = 'new_flashchips.c'
HDR_RE = re.compile(r'(const struct flashchip flashchips\[\] = \{.*?)\t\{',
                    re.MULTILINE + re.DOTALL)


def re_named(name, expr):
  return '(?P<' + name + '>' + expr + ')'


def re_top(name, capture, is_optional):
  "Regex for capturing a top level value"
  opt = '?' if is_optional else ''
  indent = r'\t\t'
  return (
      r'(?:' + re_named(name + '_pre', r'(?:' + indent + r'/\*[^}]*?\*/\n)*') +
      indent + r'\.' + name + (r'\t\t' if len(name) <= 6 else r'\t') +
      # Optional space after equal sign allows for multiline
      r'= ?' + capture + ',' + re_named(
          name + '_lc',
          r'[ \t]+(?:(?:/\*[^\n]*?\*/)|(?://[^\n]*?))') + '?' + r'\n)' + opt)


class Field:

  def __init__(self, name, is_optional):
    self.name = name
    self.is_optional = is_optional

  def buildRegexp(self):
    raise NotImplementedError()

  def reconstruct(self, flashChip):
    fd = flashChip.getFieldData(self)
    if not fd:
      return None
    return (fd.pre_comment + '\t\t.' + self.name +
            ('\t\t' if len(self.name) <= 6 else '\t') + '=' + self.format(
                fd.data) + ',' + fd.line_comment + '\n')

  def format(self, data):
    return ('' if data[0] == '\n' else ' ') + data


class StrField(Field):

  def __init__(self, name, is_optional=False):
    super().__init__(name, is_optional)

  def buildRegexp(self):
    return re_top(self.name, '"' + re_named(self.name, r'[^"\n]*?') + '"',
                  self.is_optional)

  def format(self, data):
    return super().format('"{}"'.format(data))


class ValueField(Field):

  def __init__(self, name, is_optional=False):
    super().__init__(name, is_optional)

  def buildRegexp(self):
    return re_top(self.name, re_named(self.name, r'[^,]*?'), self.is_optional)


class TestedField(Field):

  def __init__(self):
    super().__init__('tested', False)

  def buildRegexp(self):
    return re_top('tested', re_named('tested', r'(TEST_.*?|\{.*?\})'), False)


class BlockErasersField(Field):

  def __init__(self):
    super().__init__('block_erasers', True)

  def buildRegexp(self):
    return re_top(
        'block_erasers',
        re_named('block_erasers', r'(?:\n\t\t\{.*?\n\t\t\})|(?:\{\})'), True)


class VoltageField(Field):

  def __init__(self):
    super().__init__('voltage', True)

  def buildRegexp(self):
    return re_top('voltage', re_named('voltage', r'(\{[^}]*?\})'), True)


FIELDS = [
    StrField('vendor'),
    StrField('name'),
    ValueField('bustype'),
    ValueField('manufacture_id', True),
    ValueField('model_id', True),
    ValueField('total_size'),
    ValueField('page_size'),
    ValueField('feature_bits', True),
    TestedField(),
    ValueField('spi_cmd_set', True),
    ValueField('probe'),
    ValueField('probe_timing', True),
    BlockErasersField(),
    ValueField('printlock', True),
    ValueField('unlock', True),
    ValueField('write'),
    ValueField('read', True),
    ValueField('set_4ba', True),
    ValueField('read_status', True),
    ValueField('write_status', True),
    ValueField('check_access', True),
    VoltageField(),
    ValueField('gran', True),
    ValueField('wp', True),
    ValueField('wrea_override', True),
]

COMPARE_FIELDS = [f for f in FIELDS if f.name != 'wp']

ENTRY_RE = re.compile(
    r'(?P<pre_comment>(?:^\t/\*.*?\*/\n){1,2}\n?)?'
    r'(?P<if_zero>#if 0\n)?'
    r'^\t\{\n' + ''.join(f.buildRegexp() for f in FIELDS) +
    r'(?P<final_comment>(\t\t/\*.*?\*/\n)+)?'
    r'^\t\},(?P<endif>\n#endif)?\n\n', re.MULTILINE + re.DOTALL)


class FieldData:

  def __init__(self, pre_comment, data, line_comment):
    self.pre_comment = pre_comment or ''
    self.data = data
    self.line_comment = line_comment or ''


class FlashChip:

  def __init__(self, match):
    self.match = match
    if self.get('name') == DEBUG_NAME:
      for (k, v) in match.groupdict().items():
        if v is not None:
          print('{}: ({}){}'.format(k, len(v), v[:80]))
        else:
          print('{}: None'.format(k))

  def getFieldData(self, field):
    if self.get(field.name):
      return FieldData(
          self.get(field.name + '_pre'), self.get(field.name),
          self.get(field.name + '_lc'))
    else:
      # think about returning a null object
      return None

  def get(self, name):
    return self.match.group(name)

  def has(self, name):
    return bool(self.match.group(name))

  def hasFieldData(self, field):
    return self.has(field.name)

  def getLineComment(self, name):
    return self.match.group(name + "_lc")

  @property
  def size(self):
    return self.match.end()

  @property
  def nice_name(self):
    return "{} - {}".format(self.get('vendor'), self.get('name'))

  def reconstruct(self):
    result = []
    if self.has('pre_comment'):
      result.append(self.get('pre_comment'))
    if self.get('if_zero'):
      result.append('#if 0\n')
    result.append('\t{\n')
    result.extend(f.reconstruct(self) for f in FIELDS if self.hasFieldData(f))
    if self.has('final_comment'):
      result.append(self.get('final_comment'))
    result.append('\t},')
    if self.has('endif'):
      result.append('\n#endif')
    result.append('\n\n')
    return ''.join(result)

  def __repr__(self):
    return '{} {}'.format(self.get('vendor'), self.get('name'))


class MatchIterator:
  "Iterate over successive matches to a regex"

  def __init__(self, regex, text):
    self.regex = regex
    self.text = text

  def __iter__(self):
    return self

  def __next__(self):
    m = self.regex.match(self.text)
    if not m:
      raise StopIteration
    self.text = self.text[m.end():]
    return m


def countLines(text):
  return len(text.splitlines())


def keyForEntry(e):
  """Defines the sort order for entries in the file.

  The order is mostly name within vendor, although some generic entries are
  pushed to the end fo the file.
  """
  group = 0
  vendor = e.get('vendor')
  name = e.get('name')
  if vendor == 'Uknown': # This is a bug, but leave it for now.
    group += 10
  if vendor == 'Programmer':
    group += 20
  if name.startswith('unknown'):
    group += 30
  if vendor == 'Generic':
    group += 100
  return '{:05d}||{}||{}'.format(group, e.get('vendor'), e.get('name'))


class CFileContents:

  def __init__(self, text):
    "Parse by finding header, entries and footer"
    self.text = text
    # find header
    hdr_match = HDR_RE.search(self.text)
    header_end = hdr_match.end(1)
    self.header = text[:header_end]
    self.entries = [
        FlashChip(m) for m in MatchIterator(ENTRY_RE, text[header_end:])
    ]
    self.footer_start = len(self.header) + sum(e.size for e in self.entries)
    self.footer = text[self.footer_start:]

  @property
  def entries_begin_line(self):
    return countLines(self.header)

  @property
  def entries_end_line(self):
    return countLines(self.text[:self.footer_start])

  def reconstruct(self):
    result = []
    result.append(self.header)
    for e in self.entries:
      result.append(e.reconstruct())
    result.append(self.footer)
    return ''.join(result)

  def sort(self):
    self.entries.sort(key=keyForEntry)


def readCFile(dirkey):
  src = Path(DIRS[dirkey], FLASHCHIPS_C)
  with src.open() as f:
    text = f.read()
  return CFileContents(text)


ARG_PARSER = argparse.ArgumentParser(prog='python parse.py')
ARG_PARSER.set_defaults(func=None)
ARG_PARSER.add_argument('--debug_at', dest='debug_name')
SUBPARSERS = ARG_PARSER.add_subparsers()


def dump_regexen(args):

  def fix_pattern(pat):
    return '(?ms)' + re.sub(r'\?P<.*?>', '', pat)

  print('Header regex >>>')
  print(fix_pattern(HDR_RE.pattern))
  print('<<<')
  print('Entry regex >>>')
  print(fix_pattern(ENTRY_RE.pattern))
  print('<<<')


REGEX_PARSER = SUBPARSERS.add_parser('regex')
REGEX_PARSER.set_defaults(func=dump_regexen)


def summarize(args):
  cfile = readCFile(args.use)
  print("Got {} entries".format(len(cfile.entries)))
  print("Begin at line {}".format(cfile.entries_begin_line))
  print("End at line {}".format(cfile.entries_end_line))


SUMMARIZE_PARSER = SUBPARSERS.add_parser('summarize')
SUMMARIZE_PARSER.set_defaults(func=summarize)
SUMMARIZE_PARSER.add_argument(
    '--use', choices=DIRS.keys(), default='chromiumos')


def names(args):
  cfile = readCFile(args.use)
  for e in cfile.entries:
    print('{}\t{}'.format(e.get('vendor'), e.get('name')))
  print('{} Total entries'.format(len(cfile.entries)))


NAMES_PARSER = SUBPARSERS.add_parser('names')
NAMES_PARSER.set_defaults(func=names)
NAMES_PARSER.add_argument('--use', choices=DIRS.keys(), default='chromiumos')


def print_diffs(a, b, print_cb=print, a_name='A', b_name='B'):
  # print differences between two chips return number of diffs
  diffs = 0
  for field in COMPARE_FIELDS:
    aval = a.get(field.name)
    bval = b.get(field.name)
    if aval != bval:
      print_cb('{}:\n\t{}:{}\n\t{}:{}'.format(field.name, a_name, aval, b_name,
                                              bval))
      diffs += 1
  return diffs


def compare(args):
  cfile = readCFile(args.use)

  def checkArg(name, val):
    mx = len(cfile.entries) - 1
    if val < 0 or val > mx:
      print('{} must be in range 0 to {} (is {})'.format(name, mx, val))
      sys.exit(1)

  checkArg('A', args.a)
  checkArg('B', args.b)
  a = cfile.entries[args.a]
  b = cfile.entries[args.b]
  print("{} vs {}".format(a.get('name'), b.get('name')))
  diffs = print_diffs(a, b)
  print('{} fields different'.format(diffs))


COMPARE_PARSER = SUBPARSERS.add_parser('compare')
COMPARE_PARSER.add_argument('a', metavar='A', type=int)
COMPARE_PARSER.add_argument('b', metavar='B', type=int)
COMPARE_PARSER.set_defaults(func=compare)
COMPARE_PARSER.add_argument('--use', choices=DIRS.keys(), default='chromiumos')


def rewrite(args):
  cfile = readCFile(args.use)
  if args.sort:
    cfile.sort()
  dest = Path(TMP_DIR, NEW_FLASHCHIPS_C)
  with dest.open(mode='w') as f:
    f.write(cfile.reconstruct())


REWRITE_PARSER = SUBPARSERS.add_parser('rewrite')
REWRITE_PARSER.set_defaults(func=rewrite)
REWRITE_PARSER.add_argument('--sort', action='store_true')
REWRITE_PARSER.add_argument('--use', choices=DIRS.keys(), default='chromiumos')


def diff(args):
  achips = readCFile(args.A).entries
  bchips = readCFile(args.B).entries
  # Work though the two lists, which are assumed to be sorted as defined by keyForEntry()
  ab_identical = 0
  ab_with_diffs = 0
  a_only = 0
  b_only = 0
  while achips and bchips:
    akey = keyForEntry(achips[0])
    bkey = keyForEntry(bchips[0])
    if akey == bkey:
      # compare individual entry
      diff_strs = []
      diff_count = print_diffs(achips[0], bchips[0],
                               lambda x: diff_strs.append(x), args.A, args.B)
      if diff_count:
        print('{}: differ in {} fields'.format(achips[0].nice_name, diff_count))
        print('\n'.join(diff_strs))
        print()
        ab_with_diffs += 1
      else:
        ab_identical += 1
      achips = achips[1:]
      bchips = bchips[1:]
    elif akey < bkey:
      print('{}: in {} only'.format(achips[0].nice_name, args.A))
      a_only += 1
      achips = achips[1:]
    else:  # bkey < akey
      print('{}: in {} only'.format(bchips[0].nice_name, args.B))
      b_only += 1
      bchips = bchips[1:]
  for e in (achips or bchips):
    print('{}: in {} only'.format(e.nice_name, args.A if achips else args.B))
  print('\n\nSummary')
  print('=======\n')
  print("Compared {} with {}\n".format(args.A, args.B))
  print('{} chips identical'.format(ab_identical))
  print('{} chips one or more differences'.format(ab_with_diffs))
  print('{} in {} only'.format(a_only, args.A))
  print('{} in {} only'.format(b_only, args.B))


DIFF_PARSER = SUBPARSERS.add_parser('diff')
DIFF_PARSER.set_defaults(func=diff)
DIFF_PARSER.add_argument(
    'A', choices=DIRS.keys(), nargs='?', default='upstream')
DIFF_PARSER.add_argument(
    'B', choices=DIRS.keys(), nargs='?', default='chromiumos')


def csvsummary(args):
  achips = readCFile(args.A).entries
  bchips = readCFile(args.B).entries
  w = csv.writer(sys.stdout)

  # Work though the two lists, which are assumed to be sorted as defined by keyForEntry()
  while achips and bchips:
    a = achips[0]
    b = bchips[0]
    akey = keyForEntry(a)
    bkey = keyForEntry(b)
    if akey == bkey:
      # compare individual entry
      diff_count = sum(
          a.get(field.name) != b.get(field.name) for field in COMPARE_FIELDS)
      w.writerow([a.get('vendor'), a.get('name'), 'TRUE', 'TRUE', diff_count])
      achips = achips[1:]
      bchips = bchips[1:]
    elif akey < bkey:
      w.writerow([a.get('vendor'), a.get('name'), 'TRUE', 'FALSE', 0])
      achips = achips[1:]
    else:  # bkey < akey
      w.writerow([b.get('vendor'), b.get('name'), 'FALSE', 'TRUE', 0])
      bchips = bchips[1:]
  for a in achips:
    w.writerow([a.get('vendor'), a.get('name'), 'TRUE', 'FALSE', 0])
  for b in bchips:
    w.writerow([b.get('vendor'), b.get('name'), 'FALSE', 'TRUE', 0])


CSV_SUMMARY_PARSER = SUBPARSERS.add_parser('csvsummary')
CSV_SUMMARY_PARSER.set_defaults(func=csvsummary)
CSV_SUMMARY_PARSER.add_argument(
    'A', choices=DIRS.keys(), nargs='?', default='upstream')
CSV_SUMMARY_PARSER.add_argument(
    'B', choices=DIRS.keys(), nargs='?', default='chromiumos')


def main():
  global DEBUG_NAME
  args = ARG_PARSER.parse_args()
  DEBUG_NAME = args.debug_name
  if args.func:
    args.func(args)
  else:
    ARG_PARSER.print_help()


if __name__ == '__main__':
  main()
