"""Verify release material and explicit corresponding-source bundle integrity."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile
from prepare_release_sources import safe_relative

def sha256_stream(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        result.update(block)
    return result.hexdigest()

def validate(provenance, app_root=None, bundle=None, shared_assets_dir=None):
    checks = []
    def check(name, passed, detail=''):
        checks.append({'name':name, 'passed':bool(passed), 'detail':detail})
    data = json.loads(provenance.read_text(encoding='utf-8-sig'))
    version = data['version']
    check('schema', data.get('schema') == 1)
    check('no-required-source-blockers', not data.get('releaseBlockers'), json.dumps(data.get('releaseBlockers', [])))
    check('no-private-source-paths', not re.search(r'(?<![A-Za-z0-9])[A-Za-z]:[\\/]|file://', provenance.read_text(encoding='utf-8-sig')))
    names = set()
    ids = set()
    for row in data['files']:
        name = safe_relative(row['archive'])
        check('unique-file:' + row['id'], name.casefold() not in names and row['id'] not in ids)
        names.add(name.casefold()); ids.add(row['id'])
        check('digest:' + row['id'], bool(re.fullmatch('[0-9a-f]{64}', row['sha256'])))
        check('source-url:' + row['id'], row.get('sourceUrl','').startswith('https://'))
    if app_root:
        required = ['LICENSE.txt', 'licenses/CGPlay_Commercial_EULA.txt', 'licenses/THIRD_PARTY_NOTICES.txt',
                    'licenses/FFMPEG_SOURCE_OFFER.txt', f'licenses/DEPENDENCY_SOURCES_{version}.md',
                    f'licenses/DEPENDENCY_PROVENANCE_{version}.json'] + data['requiredNotices']
        for relative in required:
            path = app_root / safe_relative(relative)
            check('material:' + relative, path.is_file() and path.stat().st_size > 0)
        path = app_root / 'LICENSE.txt'
        check('application-gpl3', path.is_file() and 'GNU GENERAL PUBLIC LICENSE' in path.read_text(encoding='utf-8-sig'))
        path = app_root / f'licenses/DEPENDENCY_PROVENANCE_{version}.json'
        check('staged-provenance-identity', path.is_file() and json.loads(path.read_text(encoding='utf-8-sig')) == data)
        for row in [*data.get('binaryIdentities', []), *data.get('noticeIdentities', [])]:
            path = app_root / safe_relative(row['path'])
            if not path.is_file(): check('binary:'+row['path'], False, 'missing selected distribution binary')
            else:
                with path.open('rb') as stream: check('binary:'+row['path'], sha256_stream(stream) == row['sha256'])
    if bundle:
        from contextlib import ExitStack
        index_path = bundle.with_suffix('.assets.json')
        if index_path.is_file():
            asset_index = json.loads(index_path.read_text(encoding='utf-8-sig'))
            check('source-asset-index-version', asset_index['version'] == version and asset_index['primary'] == bundle.name)
            assets = asset_index['assets']
            shared_assets = asset_index.get('sharedAssets', [])
        else:
            assets = [{'name':bundle.name}]
            shared_assets = []
        shared_names = {row['name'] for row in shared_assets}
        assets = [*assets, *shared_assets]
        check('unique-source-assets', len(assets) == len({row['name'].casefold() for row in assets}))
        with ExitStack() as stack:
            archives = {}
            actual_inventory = set()
            for asset in assets:
                name = safe_relative(asset['name'])
                if Path(name).name != name or not name.endswith('.zip'):
                    raise ValueError('Invalid source asset name: '+name)
                shared = name in shared_names
                path = (shared_assets_dir or bundle.parent) / name if shared else bundle.parent / name
                sidecar = path.with_suffix(path.suffix+'.sha256')
                expected = sidecar.read_text(encoding='ascii').split()[0] if sidecar.is_file() else asset.get('sha256', '') if shared else ''
                with path.open('rb') as stream: sha = sha256_stream(stream)
                check('bundle-sha256:'+name, sha == expected and ('sha256' not in asset or sha == asset['sha256']))
                check('github-asset-size:'+name, path.stat().st_size < 2*1024**3)
                if 'bytes' in asset:
                    check('bundle-size:'+name, path.stat().st_size == asset['bytes'])
                archive = stack.enter_context(zipfile.ZipFile(path))
                archives[name] = archive
                members = [safe_relative(n) for n in archive.namelist() if not n.endswith('/')]
                check('unique-bundle-paths:'+name, len(members) == len({n.casefold() for n in members}))
                if shared:
                    check('shared-source-url:'+name, asset.get('url', '').startswith('https://'))
                    check('shared-source-digest:'+name, bool(re.fullmatch('[0-9a-f]{64}', asset.get('sha256', ''))))
                    selected = asset.get('members')
                    if not isinstance(selected, list) or not selected:
                        raise ValueError('Shared source asset must select explicit dependency members: '+name)
                    selected = [safe_relative(n) for n in selected]
                    if any(not n.startswith('dependencies/') for n in selected):
                        raise ValueError('Shared source assets may only supply dependency members: '+name)
                    check('shared-member-selection:'+name, len(selected) == len(set(selected)) and set(selected) <= set(members))
                    actual_inventory.update((name,n) for n in selected)
                else:
                    actual_inventory.update((name,n) for n in members if name != bundle.name or n not in ['source-inventory.json','dependency-provenance.json'])
            archive = archives[bundle.name]
            check('bundle-provenance-identity', json.loads(archive.read('dependency-provenance.json')) == data)
            inventory = json.loads(archive.read('source-inventory.json'))
            check('bundle-version', inventory['version'] == version)
            expected_inventory = {(row.get('asset',bundle.name),row['path']) for row in inventory['files']}
            check('complete-bundle-inventory', expected_inventory == actual_inventory)
            by_path = {}
            for row in inventory['files']:
                asset = row.get('asset',bundle.name)
                if asset not in archives: raise ValueError('Missing source part: '+asset)
                if row['path'] in by_path: raise ValueError('Duplicate inventory source path: '+row['path'])
                by_path[row['path']] = asset
                with archives[asset].open(safe_relative(row['path'])) as stream:
                    check('file-sha256:'+row['path'], sha256_stream(stream) == row['sha256'])
            for row in data['files']:
                member = 'dependencies/'+row['archive']
                if member not in by_path: check('dependency:'+row['id'], False, 'Missing dependency member')
                else:
                    with archives[by_path[member]].open(member) as stream:
                        check('dependency:'+row['id'], sha256_stream(stream) == row['sha256'])
            check('bundle-application-gpl3', 'GNU GENERAL PUBLIC LICENSE' in archive.read('application/LICENSE.txt').decode('utf-8-sig'))
    return {'passed':all(row['passed'] for row in checks), 'backgroundMode':True,
            'platformVisibleTopLevelWindows':0, 'version':version, 'checks':checks, 'limitations':data.get('limitations',[])}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--provenance',type=Path,required=True)
    parser.add_argument('--app-root',type=Path)
    parser.add_argument('--bundle',type=Path)
    parser.add_argument('--shared-assets-dir',type=Path,help='Local directory containing checksum-pinned shared source ZIPs; defaults to the bundle directory. No files are downloaded automatically.')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    try: report=validate(args.provenance,args.app_root,args.bundle,args.shared_assets_dir)
    except (OSError,ValueError,KeyError,zipfile.BadZipFile) as error:
        report={'passed':False,'backgroundMode':True,'platformVisibleTopLevelWindows':0,'error':str(error)}
    text=json.dumps(report,indent=2)
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(text,encoding='utf-8')
    print(text)
    raise SystemExit(0 if report['passed'] else 1)
if __name__=='__main__': main()

