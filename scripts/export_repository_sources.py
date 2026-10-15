#!/usr/bin/env python3
"""Export the current tracked and nonignored project sources through the canonical archiver."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--platform', required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()
    files = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).decode().split('\0')
    with tempfile.TemporaryDirectory(prefix='heaven experimental sources ') as temporary:
        context = Path(temporary)
        for relative in sorted(set(files) - {''}):
            source = root / relative
            if re.match(r'^(sl\..*|nvngx.*)\.dll$', source.name, re.I):
                raise ValueError('Source export must not distribute bring-your-own NVIDIA runtime plugins: ' + relative)
            if not source.exists() and not source.is_symlink():
                # A working-tree deletion is part of the exact current source.
                continue
            if not source.is_file():
                raise ValueError('Source export cannot omit a tracked path: ' + relative)
            target = context / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_symlink():
                target.symlink_to(source.readlink())
            else:
                shutil.copy2(source, target)
        subprocess.run([sys.executable, str(root / 'scripts/export_build_sources.py'),
                        '--source-root', str(context), '--output-dir', str(args.output_dir.resolve()),
                        '--platform', args.platform], check=True)
    manifest_path = args.output_dir / 'source-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['git_head'] = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    manifest['git_worktree_status'] = subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain'], text=True)
    manifest['source_policy'] = 'Exact current tracked and nonignored working-tree files; includes local edits; fetched dependencies reconstructed from checked-in pins.'
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
