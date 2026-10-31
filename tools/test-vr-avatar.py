#!/usr/bin/env python3
"""Automated synthetic-arm render check using the user's existing VR install.

Usage: python tools/test-vr-avatar.py [PAL|USA]
Runs a private flat-screen profile under work/avatar-smoke-<version>, with
synthetic controller poses, muted audio and scripted input; closes its own
window normally after activation. Disc modules are copied only locally.
This does not replace headset testing. No save from the seed profile is copied.
"""
from pathlib import Path
import os,subprocess,shutil,time,ctypes,json,sys
root=Path(__file__).resolve().parents[1]
version=sys.argv[1].upper() if len(sys.argv)>1 else 'PAL'
assert version in ('PAL','USA')
run=root/'work'/('avatar-smoke-'+version.lower())
config=run/'appdata'/"Luigi's Mansion VR"
config.mkdir(parents=True,exist_ok=True)
(run/'cache').mkdir(exist_ok=True)
(run/'cards').mkdir(exist_ok=True)
(run/'dump').mkdir(exist_ok=True)
seed=root/'profiles'/'vr'/'appdata'/"Luigi's Mansion VR"
for name in ['install.ini','game']:
 src=seed/name;dst=config/name
 if src.is_dir():shutil.copytree(src,dst,dirs_exist_ok=True)
 else:shutil.copyfile(src,dst)
if version=='USA':
 p=config/'install.ini';p.write_text(p.read_text().replace('base=PAL','base=US'))
(config/'settings.ini').write_text('[video]\ntarget_fps=60\nmatch_display_refresh=false\nresolution_scale=1\n[enhancements]\n')
env=os.environ.copy()
env.update(APPDATA=str(run/'appdata'),LM_CARD_DIR=str(run/'cards'),LM_CACHE_DIR=str(run/'cache'),LM_VR='0',LM_VR_FIRST_PERSON='0',LM_VR_AVATAR_TEST='1',LM_VR_FACE_TEST='1',LM_AUDIO='0',LM_BACKGROUND='1',LM_OFFSCREEN='1',LM_START_MAP='2',LM_START_APPEAR='6',LM_TEST_STATE='poltergust',LM_INPUT_CLOCK='frames',LM_FRAME_DUMP=f"{run/'dump'},600,900,100")
inputs='300:START*10,450:START*10,600:A*6,750:A,900:A,1050:A,1200:A,1350:A,1500:A,1650:A,2600:STICKLEFT*45,2650:STICKRIGHT*45'
log=run/'stdout.log'
with log.open('w') as output:
 proc=subprocess.Popen([str(root/'build/windows-clang/LuigisMansionRecomp.exe'),'--window','960x720','--input',inputs],cwd=root/'build/windows-clang',env=env,stdout=output,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
 (run/'pid.txt').write_text(str(proc.pid));print('Synthetic '+version+' avatar smoke PID',proc.pid,flush=True)
 start=time.monotonic();applied=False
 while proc.poll() is None and time.monotonic()-start<180:
  text=log.read_text(errors='replace')
  if '[vr avatar] applied' in text:
   applied=True;print('Live game avatar override activated.',flush=True);time.sleep(5);break
  time.sleep(1)
 # Close only the exact process created for this test, using its normal window close.
 user=ctypes.windll.user32
 callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
 def close(hwnd,param):
  pid=ctypes.c_ulong();user.GetWindowThreadProcessId(ctypes.c_void_p(hwnd),ctypes.byref(pid))
  if pid.value==proc.pid:user.PostMessageW(ctypes.c_void_p(hwnd),0x10,0,0)
  return True
 if proc.poll() is None:user.EnumWindows(callback(close),0)
 try:code=proc.wait(timeout=20)
 except subprocess.TimeoutExpired:
  print('Test process did not close normally; PID remains',proc.pid,flush=True);raise SystemExit(2)
 result={'applied':applied,'exit_code':code,'seconds':round(time.monotonic()-start,1),'log':str(log)}
 (run/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
 raise SystemExit(0 if applied and code==0 else 1)
