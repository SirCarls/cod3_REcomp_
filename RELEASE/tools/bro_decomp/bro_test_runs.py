#!/usr/bin/env python3
# [acct2-bro] Runs the recompiled game through scripted test runs, several at once, and sums up what happened.
#
# Each run gets its own folder under OUT_DIR: its own empty save folder (a saved profile changes the menu path, so
# runs that follow the same button script must start from nothing), its log, and its dumped frames. Runs share
# nothing else, so they can go side by side; each is started at low priority so builds and other work keep going.
#
# A scenario file (JSON) lists the runs:
#   {
#     "pad_scripts": {"menus_to_prologue": "27000:start:250,30000:a:250,..."},
#     "runs": [
#       {"name": "r48", "seconds": 180, "pad": "menus_to_prologue", "dump_frames": [1500, 1800],
#        "env": {"BRO_VK": "1", "BRO_NGL_LOG": "1"}}
#     ]
#   }
# "pad" names a pad script (or is one: "ms:button:hold_ms,..."; the pad clock starts at the game's first
# XInputGetState). "env" is added to the game's environment. "dump_frames" become BRO_VK_DUMP, with the frames
# written as OUT_DIR/NAME/frame_<n>.ppm.
#
# Usage:
#   bro_test_runs.py BRO_EXE GAME_DIR SCENARIO_JSON OUT_DIR [--only NAME[,NAME...]] [--parallel N]
#   BRO_EXE: the built program; GAME_DIR: the game's files (as bro takes them); OUT_DIR: where the runs go.
# Writes OUT_DIR/summary.tsv and prints it: per run its exit, time, frames reached, and counts of the log lines
# that matter (failed indirect calls, unimplemented instructions, script-thread events, C++ throws).
import argparse, json, os, re, subprocess, sys, time
from pathlib import Path

COUNTS = {
    'icall_failed': re.compile(r'\[icall\] Failed to resolve'),
    'not_code': re.compile(r'\[icall\] 0x[0-9A-F]+ is not code'),
    'unimpl': re.compile(r'\[unimpl\]'),
    'script_start': re.compile(r'\[script\] start'),
    'script_end': re.compile(r'\[script\] (end|throw ends)'),
    'cxx_throw': re.compile(r'RtlRaiseException|C\+\+ exception outside'),
    'guard': re.compile(r'\[guard\] write'),
    'crash': re.compile(r'\[crash\]'),
}
FRAME = re.compile(r'frame (\d+)')


def start_run(run, args, pads, out_dir):
    name = run['name']
    folder = out_dir / name
    save = folder / 'save'
    save.mkdir(parents=True, exist_ok=True)
    if any(save.iterdir()):
        sys.exit(f'{save} is not empty: a run starts from an empty save folder')
    env = dict(os.environ)
    env['BRO_SOUND_MUTE'] = '1'     # test runs are silent; a run can still write its mix (BRO_SOUND_WAV)
    env.update({k: str(v) for k, v in run.get('env', {}).items()})
    pad = run.get('pad')
    if pad:
        env['BRO_PAD_SCRIPT'] = pads.get(pad, pad)
    if run.get('dump_frames'):
        env['BRO_VK_DUMP'] = ','.join(str(f) for f in run['dump_frames'])
        env['BRO_VK_DUMP_PATH'] = str(folder / 'frame_%lu.ppm')
    log = open(folder / f'{name}.log', 'wb')
    low_priority = (lambda: os.nice(10)) if hasattr(os, 'nice') else None
    process = subprocess.Popen([str(args.bro_exe), str(args.game_dir), str(save)], stdout=log,
                               stderr=subprocess.STDOUT, env=env, cwd=str(folder), preexec_fn=low_priority)
    return {'run': run, 'process': process, 'log': log, 'folder': folder, 'started': time.monotonic(),
            'deadline': time.monotonic() + float(run.get('seconds', 120))}


def finish(job, timed_out):
    process = job['process']
    if timed_out and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    job['log'].close()
    job['seconds'] = time.monotonic() - job['started']
    job['exit'] = 'time limit' if timed_out else str(process.returncode)


def summarize(job):
    name = job['run']['name']
    text = (job['folder'] / f'{name}.log').read_text(errors='replace')
    row = {'name': name, 'exit': job['exit'], 'seconds': f"{job['seconds']:.0f}"}
    frames = [int(m.group(1)) for m in FRAME.finditer(text)]
    row['last_frame'] = str(max(frames)) if frames else ''
    for key, pattern in COUNTS.items():
        row[key] = str(len(pattern.findall(text)))
    tail = [line for line in text.splitlines() if line.strip()][-1:] or ['']
    row['last_line'] = tail[0][:160].replace('\t', ' ')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bro_exe', type=Path)
    parser.add_argument('game_dir', type=Path)
    parser.add_argument('scenario', type=Path)
    parser.add_argument('out_dir', type=Path)
    parser.add_argument('--only', default='', help='comma-separated run names')
    parser.add_argument('--parallel', type=int, default=2, help='runs at once (each uses a few CPU cores)')
    args = parser.parse_args()

    scenario = json.loads(args.scenario.read_text())
    pads = scenario.get('pad_scripts', {})
    runs = scenario['runs']
    if args.only:
        wanted = set(args.only.split(','))
        runs = [r for r in runs if r['name'] in wanted]
    args.out_dir.mkdir(parents=True, exist_ok=True)

    waiting, running, done = list(runs), [], []
    while waiting or running:
        while waiting and len(running) < max(1, args.parallel):
            job = start_run(waiting.pop(0), args, pads, args.out_dir)
            print(f"started {job['run']['name']}", flush=True)
            running.append(job)
        time.sleep(1)
        for job in list(running):
            timed_out = time.monotonic() > job['deadline']
            if job['process'].poll() is not None or timed_out:
                finish(job, timed_out)
                running.remove(job)
                done.append(job)
                print(f"finished {job['run']['name']}: {job['exit']}", flush=True)

    rows = [summarize(job) for job in done]
    columns = ['name', 'exit', 'seconds', 'last_frame'] + list(COUNTS) + ['last_line']
    lines = ['\t'.join(columns)] + ['\t'.join(row[c] for c in columns) for row in rows]
    (args.out_dir / 'summary.tsv').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
