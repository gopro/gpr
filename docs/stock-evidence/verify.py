#!/usr/bin/env python3
"""Verify archived experimental receipts; optionally restore their original paths.

Uses only Python's standard library, including in relocated binary packages.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', type=Path,
                        help='restore verified files below this directory')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'manifest.json').read_text())
    if manifest['schema'] != 1:
        raise ValueError('unsupported receipt manifest')
    count = 0
    total = 0
    # Verify every archive/member before writing anything. Existing files may
    # only be reused when identical, so restoring receipts cannot replace code.
    for archive in manifest['archives']:
        name = archive['file']
        if Path(name).name != name:
            raise ValueError('archive must be beside manifest')
        path = here / name
        data = path.read_bytes()
        if len(data) != archive['size'] or hashlib.sha256(data).hexdigest() != archive['sha256']:
            raise ValueError('archive digest mismatch: ' + name)
        with tarfile.open(path, 'r:gz') as tar:
            members = tar.getmembers()
            if [m.name for m in members] != [m['path'] for m in archive['members']]:
                raise ValueError('archive inventory mismatch: ' + name)
            for member, expected in zip(members, archive['members']):
                relative = PurePosixPath(member.name)
                if (not member.isfile() or relative.is_absolute() or
                        '..' in relative.parts or relative.parts[0] != 'docs'):
                    raise ValueError('invalid receipt path: ' + member.name)
                if member.mode != expected.get('mode', 0o644):
                    raise ValueError('receipt mode mismatch: ' + member.name)
                data = tar.extractfile(member).read()
                if len(data) != expected['size'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
                    raise ValueError('member digest mismatch: ' + member.name)
                if args.extract:
                    target = args.extract.joinpath(*relative.parts)
                    if not target.resolve().is_relative_to(args.extract.resolve()):
                        raise ValueError('receipt escapes destination: ' + member.name)
                    if target.exists() and (not target.is_file() or target.read_bytes() != data):
                        raise ValueError('destination differs: ' + str(target))
                count += 1
                total += len(data)
    if args.extract:
        for archive in manifest['archives']:
            with tarfile.open(here / archive['file'], 'r:gz') as tar:
                for member in tar.getmembers():
                    target = args.extract.joinpath(*PurePosixPath(member.name).parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(tar.extractfile(member).read())
                    target.chmod(member.mode)
    print(json.dumps({'source_revision': manifest['source_revision'],
                      'archives': len(manifest['archives']), 'files': count,
                      'uncompressed_bytes': total,
                      'extracted_to': str(args.extract) if args.extract else None}))


if __name__ == '__main__':
    main()
