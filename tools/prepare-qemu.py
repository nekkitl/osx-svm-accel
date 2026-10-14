#!/usr/bin/env python3
"""Apply the recorded source overlay to the exact upstream QEMU revision."""
import argparse, hashlib, json, pathlib, shutil, subprocess
ROOT = pathlib.Path(__file__).resolve().parents[1]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def git(path, *args): return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=pathlib.Path, default=ROOT/'qemu-src')
    args = ap.parse_args(); dest = args.source.resolve()
    manifest = json.loads((ROOT/'qemu/manifest.json').read_text())
    if not dest.exists():
        subprocess.run(['git','clone','--filter=blob:none','--no-checkout',manifest['upstream_url'],str(dest)],check=True)
        subprocess.run(['git','-C',str(dest),'checkout','--detach',manifest['upstream_commit']],check=True)
    if git(dest, 'rev-parse', 'HEAD') != manifest['upstream_commit']:
        raise SystemExit('Wrong upstream revision; use a separate checkout at '+manifest['upstream_commit'])
    allowed = {x['path'] for x in manifest['files']}
    changed = set(git(dest,'diff','--name-only','HEAD').splitlines())
    if changed-allowed: raise SystemExit('Checkout has unrelated changes: '+str(sorted(changed-allowed)))
    for item in manifest['files']:
        incoming = ROOT/'qemu'/item['path']; target=dest/item['path']
        if digest(incoming) != item['sha256']: raise SystemExit('Overlay checksum mismatch: '+item['path'])
        expected = {item['upstream_sha256'], item['sha256']}
        actual = digest(target) if target.exists() else None
        if actual not in expected: raise SystemExit('Refusing to overwrite unexpected source: '+item['path'])
    for item in manifest['files']:
        target=dest/item['path'];target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT/'qemu'/item['path'],target)
    print('Applied',len(manifest['files']),'files to',dest)
if __name__=='__main__': main()
