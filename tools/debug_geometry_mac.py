#!/usr/bin/env python3
import os,sys,shutil
sys.path.insert(0,os.path.abspath('tests'))
import verify_pane_palette as m
td=m.sess_mkdir('[general]\nsession = true\nmouse = true\nanim = off\n')
try:
 with open(td+'/termux.session','w') as f:
  f.write('s 2\nu 1795490000\nT 0 v50pp\nP 1 /bin/bash --noprofile --norc\nD LEFT\nP 0 /bin/bash --noprofile --norc\nD RIGHT\nT 0 h50pp\nP 0 /bin/bash --noprofile --norc\nD TOP\nP 0 /bin/bash --noprofile --norc\nD BOTTOM\n')
 log=td+'/first-size.log'; hook='printf "%s %s %s\\n" "$$" "$COLUMNS" "$LINES" >> '+log
 print('TEMP',td,'HOOK',repr(hook),flush=True)
 data,ex=m.sess_run(td,24,100,[b'echo MACDEBUG:$$:$COLUMNS:$LINES:$PROMPT_COMMAND\r'],env_extra={'PROMPT_COMMAND':hook,'TERMUX_DUMP':'1'})
 print('EX',ex,'FILE',repr(open(log).read()) if os.path.isfile(log) else 'MISSING',flush=True)
 for name in ('termux_dump.log','mouse_dump.log'):
  p=td+'/'+name
  if os.path.isfile(p):print(name,repr(open(p,'rb').read()[:6000]),flush=True)
finally:shutil.rmtree(td,ignore_errors=True)
