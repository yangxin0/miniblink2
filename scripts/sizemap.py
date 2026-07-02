#!/usr/bin/env python3
"""Attribute __text bytes of libminiblink2.dylib to engine components.

  nm -n <out>/libminiblink2.dylib | scripts/sizemap.py

Uses symbol-address gaps (Mach-O nlist has no sizes) and classifies mangled
names by namespace/prefix. Run it on the UNSTRIPPED dylib in the build out dir
(out/mono-release/libminiblink2.dylib), not the stripped dist copy — the dist
one has too few symbols for meaningful attribution. This is the measurement
step of the pruning loop: measure -> explain (gn refs / -Wl,-why_live) ->
gate behind a build-lib.sh toggle -> re-measure.
"""
import re, sys
from collections import Counter

RULES = [
    # (regex on symbol name, component)
    (r'^__ZN2v8|^_Builtins_|^_v8_|^__ZN9v8_inspec|^__ZN8cppgc', 'v8'),
    (r'^__ZNK?\d*(5blink|3WTF)|^__ZN5blink|^__ZN3WTF', 'blink'),
    (r'^__ZNK?9skia|^__ZNK?\d+Sk|^__ZN5skgpu|^__ZN4skif|^__ZN6skcms|^_sk_|^__ZNK?\d+Gr|^__ZN8SkOpts', 'skia'),
    (r'^__ZN6webrtc|^__ZN7cricket|^__ZN3rtc|^__ZN4rtc\d|^__ZN6absl.*webrtc|^__ZN3dcsctp|^_rtc_', 'webrtc'),
    (r'^__ZN5media|^__ZN5blink5media', 'media(chromium)'),
    (r'^_av_|^_ff_|^_avcodec|^_avutil|^_swr_|^_swresample|^_avformat', 'ffmpeg'),
    (r'^_vpx_|^_vp8_|^_vp9_|^_vpx', 'libvpx'),
    (r'^_dav1d|^__ZL?\d*dav1d', 'dav1d'),
    (r'^_aom_|^_av1_', 'libaom'),
    (r'^_opus_|^_silk_|^_celt_|^_ec_', 'opus'),
    (r'^__ZN6tflite|^_xnn_|^__ZN4ruy_|^__ZN4ruy', 'tflite/xnnpack'),
    (r'^__ZN3net|^__ZN4quic|^__ZN5quici|^__ZN8quiche', 'net+quic'),
    (r'^_BORINGSSL|^_SSL_|^_EVP_|^_BN_|^_X509|^_CRYPTO_|^_EC_|^_RSA_|^_ASN1|^_bn_|^_ecp_|^_sha\d|^_aes_|^_OPENSSL|^_x25519|^_CBB_|^_CBS_|^_bssl|^__ZN4bssl', 'boringssl'),
    (r'^__ZN2cc', 'cc(compositor)'),
    (r'^__ZN3gpu|^__ZN4gles|^_gl[A-Z]', 'gpu'),
    (r'^__ZN3viz', 'viz'),
    (r'^__ZN2gl\d|^__ZN2gl[A-Z_]|^__ZN2gl2|^__ZN2gl$|^__ZNK?2gl', 'ui/gl'),
    (r'^__ZN2ui|^__ZN3gfx|^__ZN7display', 'ui/gfx'),
    (r'^__ZN4base|^__ZN8partitio|^__ZN9allocator', 'base'),
    (r'^__ZN4mojo|^__ZN3IPC', 'mojo/ipc'),
    (r'^__ZN6icu_\d+|^_u[a-z_]+_\d+$|^_ucol|^_ubrk|^_ucnv|^_uloc|^_ures|^_unum|^_udat', 'icu'),
    (r'^_hb_|^_hb$', 'harfbuzz'),
    (r'^_FT_|^_ft_|^_af_|^_tt_|^_cff_|^_psh_|^_ps_', 'freetype'),
    (r'^_xml|^_xslt|^_xpath|^__xml', 'libxml/xslt'),
    (r'^_png_', 'libpng'),
    (r'^_j(peg|simd|init|copy|div)|^_jround', 'libjpeg'),
    (r'^_WebP|^_VP8|^_ShTo|^_Sharp', 'libwebp'),
    (r'^_(deflate|inflate|crc32|adler32|zlib|gz|compress2?|uncompress)', 'zlib'),
    (r'^_BrotliD|^_BrotliE', 'brotli'),
    (r'^__ZN4absl', 'absl'),
    (r'^__ZN6google8protobuf|^__ZN8protobuf', 'protobuf'),
    (r'^__ZN4dawn|^_wgpu|^__ZN5tint', 'dawn/tint'),
    (r'^__ZN7content', 'content'),
    (r'^__ZN8services|^__ZN7network|^__ZN5audio|^__ZN10device|^__ZN5webnn', 'services'),
    (r'^__ZN2mb|^_wke|^_mb[A-Z]', 'miniblink_host/wke'),
    (r'^__ZN3sql|^_sqlite3', 'sqlite'),
    (r'^__ZN7pdfium|^_FPDF|^__ZN5fpdf|^__ZN8chrome_p', 'pdfium'),
    (r'^__ZN2ax|^__ZN13accessibility|^__ZN2ui\d+AX|^__ZNK?\d*AX', 'accessibility'),
    (r'^__ZN9crashpad|^__ZN8breakpad', 'crashpad'),
    (r'^__ZN12device_signa|^__ZN6crypto', 'crypto(chromium)'),
    (r'^__ZN9perfetto|^__ZN15tracing|^__ZN5trace', 'perfetto'),
    (r'^__ZN7storage|^__ZN10leveldb|^__ZN7leveldb', 'storage/leveldb'),
    (r'^__ZN2wt|^__ZN9__gnu_cxx|^__ZNSt3__|^__ZNSt', 'libc++(inlined)'),
]
COMPILED = [(re.compile(p), c) for p, c in RULES]

def classify(name):
    for rx, comp in COMPILED:
        if rx.search(name):
            return comp
    m = re.match(r'^__ZNK?(\d+)', name)
    if m:
        n = int(m.group(1))
        rest = name[m.end():m.end()+n]
        return 'ns:' + rest
    return 'other-C/unknown'

syms = []
for line in sys.stdin:
    parts = line.split()
    if len(parts) < 3 or parts[1] not in ('t', 'T'):
        continue
    try:
        addr = int(parts[0], 16)
    except ValueError:
        continue
    syms.append((addr, parts[2]))

syms.sort()
sizes = Counter()
counts = Counter()
for i, (addr, name) in enumerate(syms):
    if i + 1 < len(syms):
        gap = syms[i+1][0] - addr
    else:
        gap = 0
    if gap < 0 or gap > (1 << 22):  # skip section boundaries / anomalies
        continue
    comp = classify(name)
    sizes[comp] += gap
    counts[comp] += 1

total = sum(sizes.values())
print(f'{"component":28} {"MB":>8} {"%":>6} {"#syms":>8}')
ns_misc = 0
for comp, sz in sizes.most_common():
    if comp.startswith('ns:') and sz < total * 0.003:
        ns_misc += sz
        continue
    print(f'{comp:28} {sz/1048576:8.2f} {100*sz/total:6.2f} {counts[comp]:8}')
print(f'{"(small namespaces)":28} {ns_misc/1048576:8.2f} {100*ns_misc/total:6.2f}')
print(f'{"TOTAL":28} {total/1048576:8.2f}')
