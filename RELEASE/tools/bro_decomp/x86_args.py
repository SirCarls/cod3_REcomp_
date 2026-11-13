#!/usr/bin/env python3
# [acct2-bro] Rough argument recovery for x86 functions Ghidra could not decompile (LTCG's custom conventions).
# Reads `objdump -d -M intel` of one function: which registers it reads before writing them (register
# arguments: eax/ecx/edx, xmm0-7), the stack arguments it reads (esp-relative offsets corrected for its own
# pushes and pops until the first call, and ebp-relative ones after `mov ebp,esp`), and its ret (ret N = the
# callee removes N bytes of arguments). Read-only. Usage: x86_args.py IMAGE START_HEX SIZE [NAME]
import re, subprocess, sys

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi'] + [f'xmm{i}' for i in range(8)]
SUB = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx', 'dx': 'edx'}


def regs_in(text):
    found = set()
    for t in re.findall(r'\b(e[a-d]x|e[sd]i|xmm[0-7]|[a-d][lhx])\b', text):
        found.add(SUB.get(t, t))
    return found


def main(image, start, size, name=''):
    s = int(start, 16)
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', f'--start-address={s}',
                          f'--stop-address={s + int(size)}', image], capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    written, read_first = set(), []
    depth, frame, seen_call = 0, False, False
    stack_args, ebp_args, rets = set(), set(), set()
    for addr, op, args in insns:
        ops = [a.strip() for a in args.split(',')] if args else []
        srcs = ops[1:] if op not in ('push', 'cmp', 'test', 'call', 'jmp') and ops else ops
        if op in ('xor', 'sub', 'pxor', 'xorps') and len(ops) == 2 and ops[0] == ops[1]:
            srcs = []
        dst_mem = ops[0] if ops and '[' in ops[0] else ''
        used = regs_in(' '.join(srcs) + ' ' + dst_mem)
        if op in ('add', 'sub', 'and', 'or', 'xor', 'adc', 'sbb', 'inc', 'dec', 'shl', 'shr', 'sar', 'imul', 'lea') and ops \
                and '[' not in ops[0] and op != 'lea':
            used |= regs_in(ops[0])  # read-modify-write
        if op == 'rep' or op.startswith('rep'):
            used |= {'ecx', 'esi', 'edi'}
        for r in REGS:
            if r in used and r not in written and r not in read_first:
                read_first.append(r)
        if ops and op not in ('push', 'cmp', 'test', 'call', 'jmp') and not op.startswith('j') and '[' not in ops[0]:
            for r in regs_in(ops[0]):
                if not (op in ('add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'shl', 'shr', 'sar') and r not in written):
                    written.add(r)
        if op == 'call':
            seen_call = True
            written |= {'eax', 'ecx', 'edx'}
        for m in re.finditer(r'\[esp\+0x([0-9a-f]+)\]', args):
            if not seen_call and not frame:
                off = int(m.group(1), 16) - depth
                if off >= 4:
                    stack_args.add(off)
        for m in re.finditer(r'\[ebp\+0x([0-9a-f]+)\]', args):
            if frame:
                ebp_args.add(int(m.group(1), 16) - 4)
        if op == 'push':
            depth += 4
        elif op == 'pop':
            depth -= 4
        elif op == 'sub' and ops[:1] == ['esp'] and ops[1].startswith('0x'):
            depth += int(ops[1], 16)
        elif op == 'add' and ops[:1] == ['esp'] and ops[1].startswith('0x'):
            depth -= int(ops[1], 16)
        elif op == 'mov' and ops == ['ebp', 'esp']:
            frame = True
        elif op == 'ret':
            rets.add(args or '0')
    regs = [r for r in read_first if r in ('eax', 'ecx', 'edx') or r.startswith('xmm')]
    print(f'{name or start}: register inputs {regs or "none"}; stack args at +{sorted(f"{o:X}" for o in stack_args | ebp_args)} '
          f'(from the return address); ret {sorted(rets)}; {len(insns)} instructions')


if __name__ == '__main__':
    main(*sys.argv[1:])
