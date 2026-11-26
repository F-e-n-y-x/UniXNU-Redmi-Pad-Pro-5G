#!/usr/bin/env python3
"""add-ruan-brightnessd.py FULL BINARY: put ruan-brightnessd (macOS brightness -> RuanBacklight) into make-macos-full-tree.py's
root tree FULL/root, as add-macos-metal.py does for the Metal plugin: /usr/libexec/ruan-brightnessd (ad-hoc re-signed, its
CDHash joins FULL/root.cdhash for the trust cache) and /System/Library/LaunchAgents/com.ruan.brightnessd.plist, both root-owned.
A plain arm64 executable: no arm64e marking (that is only for images loaded into arm64e processes)."""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN, PLIST = 'usr/libexec/ruan-brightnessd', 'System/Library/LaunchAgents/com.ruan.brightnessd.plist'


def main():
    full, binary = Path(sys.argv[1]), Path(sys.argv[2])
    stage = full / 'brightnessd-stage'
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    shutil.copy2(binary, stage / 'ruan-brightnessd')
    hashes_file = full / 'brightnessd.cdhash'
    subprocess.run([sys.executable, HERE / 'adhoc-resign.py', stage, hashes_file], check=True)
    new = hashes_file.read_text().split()
    assert len(new) == 1, f'expected one signed Mach-O (link with ld64.lld, which signs arm64 ad hoc): {new}'
    root = full / 'root'
    for rel, src in ((BIN, stage / 'ruan-brightnessd'), (PLIST, HERE / 'com.ruan.brightnessd.plist')):
        assert (root / rel).parent.is_dir(), f'{(root / rel).parent} is not in the tree'
        shutil.copy2(src, root / rel)
    entries = {l.split('\t')[3]: l for l in (full / 'root.manifest').read_text().splitlines() if l}
    entries[BIN], entries[PLIST] = f'100755\t0\t0\t{BIN}\t', f'100644\t0\t0\t{PLIST}\t'
    (full / 'root.manifest').write_text('\n'.join(entries[p] for p in sorted(entries)) + '\n')
    hashes = (full / 'root.cdhash').read_text().split()
    (full / 'root.cdhash').write_text('\n'.join(hashes + [h for h in new if h not in hashes]) + '\n')
    print(f'{BIN} + {PLIST}: CDHash {new[0]}')


if __name__ == '__main__':
    main()
