#!/usr/bin/env python3
"""Archive stable tracked/unignored source bytes before an isolated build."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('archive', type=Path)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--extract-to', type=Path)
args = parser.parse_args()
root = args.source.resolve()
if args.extract_to and args.extract_to.exists():
    parser.error('--extract-to must name a new directory')
def names():
    result = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root)
    return sorted(set(filter(None, result.decode().split('\0'))))
for attempt in range(5):
    paths = names()
    contents = {name: ((root / name).read_bytes(), (root / name).stat().st_mode & 0o777)
                for name in paths if (root / name).is_file()}
    if paths == names() and all((root / name).is_file() and (root / name).read_bytes() == data
                               for name, (data, _) in contents.items()):
        break
else:
    raise SystemExit('Sources changed repeatedly; retry after edits settle')
manifest = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            'files': {name: {'sha256': hashlib.sha256(data).hexdigest(), 'mode': mode}
                      for name, (data, mode) in contents.items()}}
with tarfile.open(args.archive, 'x:gz') as archive:
    for name, (data, mode) in contents.items():
        info = tarfile.TarInfo(name)
        info.size, info.mode = len(data), mode
        archive.addfile(info, io.BytesIO(data))
manifest['archive_sha256'] = hashlib.sha256(args.archive.read_bytes()).hexdigest()
args.archive.with_suffix(args.archive.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
if args.extract_to:
    args.extract_to.mkdir(parents=True)
    for name, (data, mode) in contents.items():
        path = args.extract_to / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
print(json.dumps({'archive': str(args.archive), 'sha256': manifest['archive_sha256'], 'files': len(contents)}))
