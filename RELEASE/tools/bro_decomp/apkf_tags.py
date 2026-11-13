#!/usr/bin/env python3
# [acct2-bro] CoD3's NGL packages: which kinds of data each APKF chunk in the KAPF archives lists. An APKF chunk
# starts "APKF" and is a directory of typed entries; each entry's type is 4 characters stored as a big-endian word,
# so they read backwards in the file ("HSEM" = MESH, "\0TAM" = MAT, "\0XET" = TEX, "LEKS" = SKEL, "SPEA" = AEPS).
# This counts the types in each chunk's first 1 KB. (Big Red One's counterpart is the XBXM file, see
# xbxm_sections.py; its material / mesh / texture entries are the same idea in another layout.)
# Read-only. Usage: apkf_tags.py SURVEY_TSV ASSETS_DIR OUT_TXT   (SURVEY_TSV from bro_kapf_survey.py --out)
import collections, glob, os, re, sys

TYPE = re.compile(rb'([A-Z]{3,4})(?=\x00\x00\x00[\x00-\x0f])')


def main(survey, assets, out):
    full = {}
    for p in glob.glob(os.path.join(assets, '**', '*.cod'), recursive=True):
        rel = os.path.relpath(p, assets).replace(os.sep, '/')
        full[rel] = p
        full['/'.join(rel.split('/')[1:])] = p          # the survey names some archives without sp/ or mp/
    rows = [l.rstrip('\n').split('\t') for l in open(survey, encoding='utf-8')][1:]
    types, combos, kinds = collections.Counter(), collections.Counter(), collections.Counter()
    chunks = unresolved = 0
    for r in rows:
        kinds[r[6]] += 1
        if r[6] != 'APKF':
            continue
        path = full.get(r[0])
        if not path:
            unresolved += 1
            continue
        with open(path, 'rb') as f:
            f.seek(int(r[3], 0))
            head = f.read(min(int(r[4], 0), 0x400))
        chunks += 1
        seen = {m.group(1)[::-1].decode() for m in TYPE.finditer(head)}
        types.update(seen)
        combos[' '.join(sorted(seen))] += 1
    lines = [f'chunk kinds (first four bytes): {dict(kinds.most_common(8))}',
             f'APKF chunks read: {chunks} (names not resolved: {unresolved})',
             'chunks listing each type: ' + ', '.join(f'{t} {n}' for t, n in types.most_common(12)),
             'commonest combinations: ' + '; '.join(f'[{c}] {n}' for c, n in combos.most_common(8))]
    open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main(*sys.argv[1:])
