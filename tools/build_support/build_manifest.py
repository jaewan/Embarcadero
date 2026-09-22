#!/usr/bin/env python3
"""Record executed build inputs, package versions and binary hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--source-archive', type=Path)
args = parser.parse_args()
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def command(argv, cwd=None):
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    return {'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
root = Path(__file__).resolve().parents[2]
build = args.build.resolve()
cache = build / 'CMakeCache.txt'
if cache.is_file():
    for line in cache.read_text().splitlines():
        if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL='):
            root = Path(line.split('=', 1)[1])
manifest = {'schema_version': 1, 'build_directory': str(build),
            'source_directory': str(root),
            'source_revision': command(['git', 'rev-parse', 'HEAD'], root),
            'source_status': command(['git', 'status', '--porcelain'], root),
            'compiler': command(['c++', '--version']),
            'packages': command(['dpkg-query', '-W', '-f=${Package}\t${Version}\n']),
            'inputs': {}, 'binaries': {}, 'dependency_licenses': {}}
for name in ('libfmt-dev', 'libgoogle-glog-dev', 'libgflags-dev', 'libyaml-cpp-dev',
             'libmimalloc-dev', 'libcxxopts-dev', 'libboost-dev', 'libnuma-dev'):
    path = Path('/usr/share/doc') / name / 'copyright'
    if path.is_file():
        manifest['dependency_licenses'][name] = {'path': str(path), 'sha256': digest(path), 'text': path.read_text()}
manifest['source_dependencies'] = {}
grpc_source = build / '_deps/grpc-src'
if cache.is_file():
    for line in cache.read_text().splitlines():
        if line.startswith('FETCHCONTENT_SOURCE_DIR_GRPC:PATH=') and line.split('=', 1)[1]:
            grpc_source = Path(line.split('=', 1)[1])
for name, source in (('grpc', grpc_source), ('folly', Path('/opt/embarcadero-deps/folly'))):
    if source.is_dir():
        dependency = {'path': str(source), 'revision': command(['git', 'rev-parse', 'HEAD'], source),
                      'status': command(['git', 'status', '--porcelain'], source),
                      'submodules': command(['git', 'submodule', 'status', '--recursive'], source), 'licenses': {}}
        for path in list(source.glob('LICENSE*')) + list(source.glob('COPYING*')):
            if path.is_file():
                dependency['licenses'][path.name] = {'sha256': digest(path), 'text': path.read_text(errors='replace')}
        manifest['source_dependencies'][name] = dependency
for name in ('CMakeCache.txt', 'compile_commands.json', 'test-inventory.txt'):
    path = build / name
    if path.is_file(): manifest['inputs'][name] = {'sha256': digest(path), 'text': path.read_text()}
for path in sorted((build / 'bin').glob('*')):
    if path.is_file(): manifest['binaries'][path.name] = {'sha256': digest(path), 'ldd': command(['ldd', str(path)])}
if args.source_archive:
    manifest['source_archive'] = {'path': str(args.source_archive.resolve()), 'sha256': digest(args.source_archive)}
    source_inventory = args.source_archive.with_suffix(args.source_archive.suffix + '.json')
    if source_inventory.is_file():
        manifest['source_archive']['inventory'] = json.loads(source_inventory.read_text())
args.output.write_text(json.dumps(manifest, indent=2) + '\n')
