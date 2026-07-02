#!/usr/bin/env python3
"""trim_icu.py — strip unneeded locale data from an ICU common data file (icudtl.dat).

  scripts/trim_icu.py IN.dat OUT.dat [--keep root,en,zh] [--drop-cjdict] [--dry-run]

Works directly on the .dat package (no icupkg needed): the file is a header +
sorted TOC of (name, offset) pairs + item blobs. We drop locale-shaped items in
the display/collation categories and rewrite the package.

What is dropped: per-locale bundles (xx.res / xx_YY.res) NOT in the keep set,
only inside the locale-parameterized categories: coll/ zone/ curr/ lang/ unit/
region/ rbnf/ translit/ and the top-level locale .res bundles. ICU falls back
to root for a missing bundle, so pages in other locales still render — they
just lose locale-tailored collation and display names (Intl.Collator etc.).

What is always kept: everything non-locale-shaped (pool.res string pools —
REQUIRED by remaining bundles, res_index.res, supplementalData.res,
likelySubtags.res, ucadata, *.nrm normalization, *.cnv converters, cnvalias),
and the whole brkitr/ category (line/word breaking; cjdict.dict is the CJK
segmentation dictionary — dropping it degrades Chinese/Japanese word
selection and Intl.Segmenter, so it needs the explicit --drop-cjdict).
"""
import re
import struct
import sys

LOCALE_CATEGORIES = {'coll', 'zone', 'curr', 'lang', 'unit', 'region',
                     'rbnf', 'translit'}
# xx.res / xxx.res / xx_Anything.res (root.res matches too — protected by keep set)
LOCALE_RE = re.compile(r'^[a-z]{2,3}(_[A-Za-z0-9]+)*\.res$|^root\.res$')


def parse(buf):
    header_size, = struct.unpack('<H', buf[0:2])
    base = header_size
    count, = struct.unpack('<I', buf[base:base + 4])
    toc = [struct.unpack('<II', buf[base + 4 + i * 8: base + 12 + i * 8])
           for i in range(count)]
    items = []
    for i, (name_off, data_off) in enumerate(toc):
        end = toc[i + 1][1] + base if i + 1 < count else len(buf)
        name_end = buf.index(b'\0', base + name_off)
        name = buf[base + name_off:name_end].decode()
        items.append((name, buf[base + data_off:end]))
    return buf[:header_size], items


def build(header, items):
    base_rel = 4 + 8 * len(items)          # TOC region, relative to base
    names = bytearray()
    name_offs = []
    for name, _ in items:
        name_offs.append(base_rel + len(names))
        names += name.encode() + b'\0'
    data_start = base_rel + len(names)
    out_data = bytearray()
    data_offs = []
    for _, blob in items:
        pos = data_start + len(out_data)
        pad = (-pos) % 16                   # keep every item 16-byte aligned
        out_data += b'\0' * pad
        data_offs.append(pos + pad)
        out_data += blob
    out = bytearray(header)
    out += struct.pack('<I', len(items))
    for n, d in zip(name_offs, data_offs):
        out += struct.pack('<II', n, d)
    out += names + out_data
    return bytes(out)


def keeps(name, keep, drop_cjdict):
    # name is like 'icudt78l/coll/zh_Hans.res' or 'icudt78l/de.res'
    parts = name.split('/')[1:]             # drop the icudtXXl/ prefix
    if drop_cjdict and parts == ['brkitr', 'cjdict.dict']:
        return False
    if parts[-1] == 'res_index.res':
        return True                         # locale enumeration index ('res' would
                                            # false-match LOCALE_RE as a locale)
    if len(parts) == 2 and parts[0] in LOCALE_CATEGORIES:
        leaf = parts[1]
    elif len(parts) == 1:
        leaf = parts[0]
    else:
        return True                         # brkitr/, unknown dirs: keep
    if not LOCALE_RE.match(leaf):
        return True                         # pool.res, res_index.res, *.cnv, ...
    locale = leaf[:-4].split('_')[0]
    return locale in keep


def main():
    args = sys.argv[1:]
    keep = {'root', 'en', 'zh'}
    drop_cjdict = dry_run = False
    paths = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--keep':
            keep = {'root'} | {k.strip().split('_')[0].split('-')[0]
                               for k in args[i + 1].split(',') if k.strip()}
            i += 1
        elif a == '--drop-cjdict':
            drop_cjdict = True
        elif a == '--dry-run':
            dry_run = True
        else:
            paths.append(a)
        i += 1
    if len(paths) != 2:
        sys.exit(__doc__.strip())

    buf = open(paths[0], 'rb').read()
    header, items = parse(buf)
    kept = [(n, b) for n, b in items if keeps(n, keep, drop_cjdict)]
    dropped = len(items) - len(kept)
    out = build(header, kept)
    print(f'trim_icu: keep={{{",".join(sorted(keep))}}}'
          f'{" -cjdict" if drop_cjdict else ""}: '
          f'{len(items)} items / {len(buf) / 1048576:.1f}MB -> '
          f'{len(kept)} items / {len(out) / 1048576:.1f}MB '
          f'({dropped} dropped)')
    if not dry_run:
        open(paths[1], 'wb').write(out)


if __name__ == '__main__':
    main()
