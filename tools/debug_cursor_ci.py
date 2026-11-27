#!/usr/bin/env python3
"""临时诊断 runner 的 shell 输入/输出，不并入主线。"""
import os, shutil, sys, tempfile
sys.path.insert(0, os.path.abspath('tests'))
import verify_pane_palette as m
print('ENV', {k: os.environ.get(k) for k in ('HOME','LANG','LC_ALL','TERM','COLUMNS','LINES','INPUTRC','USER')}, flush=True)
out = '/tmp/cursor-fast'; os.makedirs(out, exist_ok=True)
for focus in (1,0):
    td = m.sess_mkdir('[general]\nsession = true\nmouse = true\nanim = off\n')
    try:
        with open(os.path.join(td,'termux.session'),'w') as f:
            f.write('s 2\nu 1795490000\nT 0 v50pp\nP %d /bin/bash --noprofile --norc\nD CURSORLEFT\nP %d /bin/bash --noprofile --norc\nD CURSORRIGHT\n' % (focus,1-focus))
        data, ex = m.sess_run(td,24,100,[b'#XYZ'+b'a'*55,b'\x01',b'\x1b[C'*4],env_extra={'TERMUX_DUMP':'1','TERMUX_CURSOR_DIAG':'1'})
        result=m.vt_cursor_check(24,100,data,'#XYZ'); print('FOCUS',focus,'RESULT',result,ex,flush=True)
        with open(os.path.join(out,'focus%d.bin'%focus),'wb') as f:f.write(data)
        for name in ('mouse_dump.log','termux_dump.log','render_dump.log','render_delta.log'):
            src=os.path.join(td,name)
            if os.path.isfile(src): shutil.copy2(src,os.path.join(out,'focus%d-%s'%(focus,name)))
    finally: shutil.rmtree(td,ignore_errors=True)
for focus in (1,0):
    td=m.sess_mkdir('[general]\nsession = true\nmouse = true\nanim = off\n')
    try:
        with open(os.path.join(td,'termux.session'),'w') as f:
            f.write('s 2\nu 1795490000\nT 0 v50pp\nP %d /bin/bash --noprofile --norc\nD CURSORLEFT\nP %d /bin/bash --noprofile --norc\nD CURSORRIGHT\n' % (focus,1-focus))
        data, ex=m.sess_run(td,24,100,[b'echo BASHCOLUMNS=$COLUMNS; stty size\r',b'#XYZ'+b'a'*55,b'\x01',b'\x1b[C'*4],env_extra={'TERMUX_DUMP':'1','TERMUX_CURSOR_DIAG':'1'})
        print('WITH_DIM',focus,m.vt_cursor_check(24,100,data,'#XYZ'),ex,flush=True)
        for name in ('mouse_dump.log','termux_dump.log'):
            src=os.path.join(td,name)
            if os.path.isfile(src):shutil.copy2(src,os.path.join(out,'dim%d-%s'%(focus,name)))
    finally:shutil.rmtree(td,ignore_errors=True)
for iteration in range(8):
    td=m.sess_mkdir('[general]\nsession = true\nmouse = true\nanim = off\n')
    try:
        with open(os.path.join(td,'termux.session'),'w') as f:
            f.write('s 2\nu 1795490000\nT 0 v50pp\nP 0 /bin/bash --noprofile --norc\nD CURSORLEFT\nP 1 /bin/bash --noprofile --norc\nD CURSORRIGHT\n')
        data, ex=m.sess_run(td,24,100,[b'#XYZ'+b'a'*55,b'\x01',b'\x1b[C'*4],env_extra={'TERMUX_DUMP':'1','TERMUX_CURSOR_DIAG':'1','PS1':'${COLUMNS}> '})
        result=m.vt_cursor_check(24,100,data,'#XYZ')
        raw=open(os.path.join(td,'termux_dump.log'),'rb').read()
        print('PROMPT_RUN',iteration,'RESULT',result,'RAW',repr(raw[:500]),flush=True)
        for name in ('mouse_dump.log','termux_dump.log'):
            src=os.path.join(td,name)
            if os.path.isfile(src):shutil.copy2(src,os.path.join(out,'prompt%d-%s'%(iteration,name)))
    finally:shutil.rmtree(td,ignore_errors=True)
