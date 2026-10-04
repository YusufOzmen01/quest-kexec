#!/usr/bin/env python3
"""Quarantine alternate-image OTA entrypoints and crash-looping CMS APK only.
Never opens a block device. Back up/unshare before editing Android ext4 images.
"""
from pathlib import Path
import hashlib, subprocess, tempfile
ROOT=Path('/home/yusuf/kexectest/work')
IMAGES=ROOT/'no-usb-images'
FILES={
 'system': ['/system/etc/init/update_engine.rc', '/system/bin/update_engine',
            '/system/bin/update_engine_client', '/system/bin/update_verifier', '/system/bin/postinstall'],
 'system_ext': ['/etc/init/update_engine_gold.rc', '/priv-app/OSUpdater/OSUpdater.apk',
                '/priv-app/NuxOta/NuxOta.apk', '/priv-app/CMSHeadset/CMSHeadset.apk'],
}

def fsck(img, *args):
 p=subprocess.run(['e2fsck',*args,str(img)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 print(p.stdout.splitlines()[-1])
 if p.returncode not in (0,1): raise RuntimeError(p.stdout)

def debug(img, cmd, write=False):
 p=subprocess.run(['debugfs',*(['-w'] if write else []),'-R',cmd,str(img)],capture_output=True,text=True)
 if p.returncode or any(x in p.stderr.lower() for x in ['could not allocate','filesystem full','no space']):
  raise RuntimeError(p.stdout+p.stderr)
 return p.stdout+p.stderr

with tempfile.TemporaryDirectory(prefix='qkx-disable-') as tmp:
 tmp=Path(tmp)
 label=tmp/'label'; label.write_bytes(b'u:object_r:system_file:s0\0')
 for name, paths in FILES.items():
  img=(IMAGES/f'{name}.img').resolve()
  backup=ROOT/f'{name}-pre-disable-updater-cm.img'
  if backup.exists(): raise RuntimeError(f'Refusing to overwrite backup {backup}')
  subprocess.run(['cp','--reflink=auto',str(img),str(backup)],check=True)
  with img.open('r+b') as f: f.truncate(img.stat().st_size+(256<<20))
  fsck(img,'-fy')
  subprocess.run(['resize2fs',str(img)],check=True)
  fsck(img,'-fy','-E','unshare_blocks')
  debug(img,'mkdir /qkx-disabled',True)
  for path in paths:
   src=tmp/Path(path).name
   debug(img,f'dump {path} {src}')
   if not src.exists() or not src.stat().st_size: raise RuntimeError(f'Missing {path}')
   digest=hashlib.sha256(src.read_bytes()).hexdigest()
   dest='/qkx-disabled/'+src.name+'.disabled'
   debug(img,f'write {src} {dest}',True)
   debug(img,f'set_inode_field {dest} mode 0100600',True)
   debug(img,f'set_inode_field {dest} uid 0',True)
   debug(img,f'set_inode_field {dest} gid 0',True)
   debug(img,f'ea_set -f {label} {dest} security.selinux',True)
   verify=tmp/'verify'
   debug(img,f'dump {dest} {verify}')
   assert hashlib.sha256(verify.read_bytes()).hexdigest()==digest, path
   debug(img,f'rm {path}',True)
   assert 'File not found' in debug(img,f'stat {path}'), path
   print('QUARANTINED',name,path,digest)
  p=subprocess.run(['e2fsck','-fn',str(img)],capture_output=True,text=True)
  if p.returncode: raise RuntimeError(p.stdout+p.stderr)
  print('CLEAN',img)
