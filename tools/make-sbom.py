"""Writes the SPDX 2.3 SBOM of SC4RenderServices.dll.

A scanner finds nothing in the DLL, since its libraries are compiled in. This reads the vendored
submodules the build used instead.

    python tools/make-sbom.py --dll <SC4RenderServices.dll> --version <tag> --output <file.spdx.json>
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = 'https://github.com/caspervg/sc4-render-services'

# Submodules compiled into the DLL: path, name, licence.
SUBMODULES = [
    ('vendor/gzcom-dll', 'gzcom-dll', 'LGPL-2.1-or-later'),
    ('vendor/imgui', 'Dear ImGui', 'MIT'),
    ('vendor/spdlog', 'spdlog', 'MIT'),
    ('vendor/mINI', 'mINI', 'MIT'),
    ('vendor/Clipper2', 'Clipper2', 'BSL-1.0'),
    ('vendor/earcut', 'earcut.hpp', 'ISC'),
]
FMT_HEADER = Path('vendor/spdlog/include/spdlog/fmt/bundled/base.h')


def git(*args, cwd='.'):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def spdx_id(name):
    return 'SPDXRef-Package-' + re.sub(r'[^A-Za-z0-9.-]', '-', name)


def package(name, version, licence, location, purl=None, comment=None):
    pkg = {
        'SPDXID': spdx_id(name),
        'name': name,
        'versionInfo': version,
        'downloadLocation': location,
        'filesAnalyzed': False,
        'licenseConcluded': licence,
        'licenseDeclared': licence,
        'copyrightText': 'NOASSERTION',
    }
    if purl:
        pkg['externalRefs'] = [{'referenceCategory': 'PACKAGE-MANAGER', 'referenceType': 'purl',
                                'referenceLocator': purl}]
    if comment:
        pkg['comment'] = comment
    return pkg


def submodule(path, name, licence):
    url = git('config', '-f', '.gitmodules', '--get', f'submodule.{path}.url').removesuffix('.git')
    commit = git('rev-parse', 'HEAD', cwd=path)
    owner_repo = url.removeprefix('https://github.com/').lower()
    return package(name, commit, licence, f'git+{url}@{commit}', f'pkg:github/{owner_repo}@{commit}')


def bundled_fmt():
    version = int(re.search(r'#define FMT_VERSION (\d+)', FMT_HEADER.read_text(encoding='utf-8'))[1])
    version = f'{version // 10000}.{version // 100 % 100}.{version % 100}'
    return package('fmt', version, 'MIT', f'git+https://github.com/fmtlib/fmt@{version}',
                   f'pkg:github/fmtlib/fmt@{version}', 'Bundled in spdlog.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dll', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()

    commit = git('rev-parse', 'HEAD')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH') or git('show', '-s', '--format=%ct'))
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    dll = args.dll.read_bytes()

    root = package('SC4RenderServices', args.version.removeprefix('v'), 'LGPL-2.1-only', f'git+{REPO}@{commit}',
                   f'pkg:github/caspervg/sc4-render-services@{commit}')
    root['packageFileName'] = args.dll.name
    root['checksums'] = [{'algorithm': 'SHA256', 'checksumValue': hashlib.sha256(dll).hexdigest()},
                         {'algorithm': 'SHA1', 'checksumValue': hashlib.sha1(dll).hexdigest()}]
    root['primaryPackagePurpose'] = 'LIBRARY'

    contained = [submodule(*s) for s in SUBMODULES] + [bundled_fmt()]
    runtime = package('Microsoft Visual C++ Redistributable', 'NOASSERTION', 'NOASSERTION', 'NOASSERTION',
                      comment='Needed at run time: the DLL links the C runtime dynamically.')

    relationships = [{'spdxElementId': 'SPDXRef-DOCUMENT', 'relationshipType': 'DESCRIBES',
                      'relatedSpdxElement': root['SPDXID']}]
    relationships += [{'spdxElementId': root['SPDXID'], 'relationshipType': 'CONTAINS',
                       'relatedSpdxElement': p['SPDXID']} for p in contained]
    relationships.append({'spdxElementId': root['SPDXID'], 'relationshipType': 'DEPENDS_ON',
                          'relatedSpdxElement': runtime['SPDXID']})

    doc = {
        'spdxVersion': 'SPDX-2.3',
        'dataLicense': 'CC0-1.0',
        'SPDXID': 'SPDXRef-DOCUMENT',
        'name': f'SC4RenderServices-{args.version}',
        'documentNamespace': f'{REPO}/spdx/{args.version}-{commit}',
        'creationInfo': {'created': created, 'creators': ['Tool: sc4-render-services tools/make-sbom.py']},
        'packages': [root, *contained, runtime],
        'relationships': relationships,
    }
    args.output.write_text(json.dumps(doc, indent=2) + '\n', encoding='utf-8')
    print(f'{args.output}: {len(contained)} contained packages')


if __name__ == '__main__':
    main()
