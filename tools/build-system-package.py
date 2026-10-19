#!/usr/bin/env python3
"""Build the distributable alternate-Android ZIP and hash manifest."""
import argparse, hashlib, json, os, tempfile, zipfile
from pathlib import Path
from board import load_board

FILES = {
    'images/system.img': 'system.img', 'images/system_ext.img': 'system_ext.img',
    'images/vendor.img': 'vendor.img', 'images/odm.img': 'odm.img',
    'images/product.img': 'product.img',
}

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(8<<20), b''): h.update(b)
    return h.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('images',type=Path); p.add_argument('output',type=Path)
    p.add_argument('--kernel',type=Path,required=True); p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--board',default='seacliff')
    a=p.parse_args(); repo=a.repo.resolve(); board=load_board(a.board)
    sources={arc:(a.images/src).resolve() for arc,src in FILES.items()}
    sources.update({'kernel/Image':a.kernel.resolve(),
      'runtime/busybox':(repo/'out/busybox').resolve(),
      'modules/quest_kexec.ko':repo/'module/quest_kexec.ko',
      'modules/ion_secmap.ko':repo/'module/ion_secmap.ko',
      'modules/marker_read.ko':repo/'module/marker_read.ko'})
    for arc,path in sources.items():
        if not path.is_file(): raise SystemExit(f'missing {arc}: {path}')
    turnkey=a.images/'qkx-turnkey.json'
    if not turnkey.is_file(): raise SystemExit('images are not a turnkey set')
    sources['qkx-turnkey.json']=turnkey
    manifest={'format':'qkx-system-package-v1','device':board['QKX_BOARD_LABEL'],
              'calibration':'read-current-stock-at-each-boot','files':{}}
    for arc,path in sources.items():
        manifest['files'][arc]={'size':path.stat().st_size,'sha256':digest(path)}
        print('HASHED',arc)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    tmp=a.output.with_suffix(a.output.suffix+'.tmp'); tmp.unlink(missing_ok=True)
    with zipfile.ZipFile(tmp,'w',allowZip64=True) as z:
        z.writestr('manifest.json',json.dumps(manifest,indent=2)+'\n',compress_type=zipfile.ZIP_DEFLATED)
        for arc,path in sources.items():
            # Android sparse/ext4 images compress well; binaries do not need maximum CPU cost.
            z.write(path,arc,compress_type=zipfile.ZIP_DEFLATED,compresslevel=3)
    os.replace(tmp,a.output)
    print(a.output, digest(a.output))
if __name__=='__main__': main()
