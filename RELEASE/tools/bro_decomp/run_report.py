#!/usr/bin/env python3
# [acct2-bro] Summary of one run of the recompiled Big Red One (the program's stderr log), and the functions it found
# missing, written as a run log that xboxrecomp's tools.seed_from_log reads.
#
# Counts what matters when comparing runs: indirect calls to addresses with no function (both wordings the runtime
# uses: "[icall] no function at 0x..." and "Failed to resolve VA 0x..."), calls through pointers that aren't code,
# untranslated instructions reached, NGL / Direct3D / DirectSound stand-in calls by name, kernel calls, file reads
# (count and bytes), files not found, and how the run ended (the last lines).
#
# Read-only. Usage: run_report.py RUN_LOG [OUT_SEED_LOG]
#   then: python -m tools.seed_from_log OUT_SEED_LOG default.xbe --functions FUNCTIONS_JSON --seeds SEEDS_JSON
import collections, re, sys

MISSING = re.compile(r'(?:no function at|Failed to resolve VA) 0x([0-9A-Fa-f]{8})')
NOT_CODE = re.compile(r'0x([0-9A-Fa-f]{8}) is not code: skipped (\d+)')
UNIMPL = re.compile(r'\[unimpl\] `([^`]*)` at 0x([0-9A-Fa-f]{8})')
STAND_IN = re.compile(r'^\[ngl\] (\w+)\(')
KERNEL = re.compile(r'\[KERNEL\] (Nt\w+|Ke\w+|Mm\w+|Ex\w+|Io\w+|Rtl\w+|Xe\w+|Hal\w+|Ob\w+|Ps\w+|Av\w+)')
FILE_FAIL = re.compile(r'\[FILE\] (.*?) \(disp \d+\) -> 0x[0-9A-Fa-f]+ FAILED')
READ = re.compile(r'\[READ\] .*? got=(\d+)')     # the bridge logs file reads this way, not as [KERNEL] NtReadFile


def main(log_path, seed_out=None):
    with open(log_path, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    missing, not_code, unimpl = collections.Counter(), {}, collections.Counter()
    stand_ins, kernel, file_fail = collections.Counter(), collections.Counter(), collections.Counter()
    reads, read_bytes = 0, 0
    for line in lines:
        if m := MISSING.search(line):
            missing[int(m.group(1), 16)] += 1
        if m := NOT_CODE.search(line):
            not_code[int(m.group(1), 16)] = max(not_code.get(int(m.group(1), 16), 0), int(m.group(2)))
        if m := UNIMPL.search(line):
            unimpl[(int(m.group(2), 16), m.group(1))] += 1
        if m := STAND_IN.match(line):
            stand_ins[m.group(1)] += 1
        if m := KERNEL.search(line):
            kernel[m.group(1)] += 1
        if m := FILE_FAIL.search(line):
            file_fail[m.group(1)] += 1
        if m := READ.search(line):
            reads += 1
            read_bytes += int(m.group(1))

    out = [f'{log_path}: {len(lines)} lines']
    out.append(f'missing functions: {len(missing)} addresses, {sum(missing.values())} reports')
    out += [f'  0x{a:08X} x{n}' for a, n in missing.most_common(20)]
    out.append(f'calls through pointers that are not code: {len(not_code)} addresses')
    out += [f'  0x{a:08X} skipped {n}+ times' for a, n in sorted(not_code.items(), key=lambda x: -x[1])[:10]]
    out.append(f'untranslated instructions reached: {len(unimpl)}')
    out += [f'  0x{a:08X} `{t}`' for (a, t), _ in unimpl.most_common(10)]
    out.append(f'stand-in calls: {sum(stand_ins.values())} to {len(stand_ins)} functions')
    out += [f'  {n:6d} {name}' for name, n in stand_ins.most_common(15)]
    out.append(f'kernel calls by name: ' + ', '.join(f'{k} {n}' for k, n in kernel.most_common(12)))
    out.append(f'file reads: {reads}, {read_bytes} bytes')
    out.append(f'files not found: {len(file_fail)}')
    out += [f'  {p} x{n}' for p, n in file_fail.most_common(10)]
    out.append('last lines:')
    out += ['  ' + l[:200] for l in lines[-6:]]
    print('\n'.join(out))

    if seed_out:
        with open(seed_out, 'w', encoding='utf-8') as f:
            for a in sorted(missing):
                f.write(f'Failed to resolve VA 0x{a:08X} (run: {missing[a]} reports)\n')
        print(f'{len(missing)} missing addresses written to {seed_out}')


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit('usage: run_report.py RUN_LOG [OUT_SEED_LOG]')
    main(*sys.argv[1:])
