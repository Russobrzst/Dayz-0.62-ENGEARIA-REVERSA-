"""Two independent, read-only debug viewers for the owned DayZ test processes."""
import argparse,ctypes,json,re,time,traceback
from ctypes import wintypes
from collections import deque
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent

class Tail:
    def __init__(self,path):
        self.path=Path(path);self.offset=0;self.partial=b'';self.started=False
    def read(self):
        if not self.path.exists():return []
        size=self.path.stat().st_size
        historical=not self.started
        if not self.started:
            self.offset=max(0,size-65536);self.started=True
        if size<self.offset:self.offset=0;self.partial=b''
        with self.path.open('rb') as stream:
            stream.seek(self.offset);data=stream.read(65536);self.offset=stream.tell()
        lines=(self.partial+data).split(b'\n');self.partial=lines.pop()
        decoded=[line.decode('utf-8',errors='replace').rstrip('\r') for line in lines]
        if historical and self.path.name=='script.log':
            starts=[i for i,line in enumerate(decoded) if line.startswith('Log ')]
            if starts:decoded=decoded[starts[-1]:]
        return [(line,historical) for line in decoded if line.strip()]

class MODULEENTRY32W(ctypes.Structure):
    _fields_=[('dwSize',wintypes.DWORD),('th32ModuleID',wintypes.DWORD),('th32ProcessID',wintypes.DWORD),
      ('GlblcntUsage',wintypes.DWORD),('ProccntUsage',wintypes.DWORD),('modBaseAddr',ctypes.POINTER(ctypes.c_byte)),
      ('modBaseSize',wintypes.DWORD),('hModule',wintypes.HMODULE),('szModule',wintypes.WCHAR*256),('szExePath',wintypes.WCHAR*260)]

class PROCESSENTRY32W(ctypes.Structure):
    _fields_=[('dwSize',wintypes.DWORD),('cntUsage',wintypes.DWORD),('th32ProcessID',wintypes.DWORD),
      ('th32DefaultHeapID',ctypes.c_size_t),('th32ModuleID',wintypes.DWORD),('cntThreads',wintypes.DWORD),
      ('th32ParentProcessID',wintypes.DWORD),('pcPriClassBase',ctypes.c_long),('dwFlags',wintypes.DWORD),
      ('szExeFile',wintypes.WCHAR*260)]

def owned_modules(pid,expected_exe):
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];k.OpenProcess.restype=wintypes.HANDLE
    k.CloseHandle.argtypes=[wintypes.HANDLE];k.CloseHandle.restype=wintypes.BOOL
    k.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    k.QueryFullProcessImageNameW.restype=wintypes.BOOL
    process=k.OpenProcess(0x1000,False,pid)
    if not process:return None
    try:
        buffer=ctypes.create_unicode_buffer(32768);length=wintypes.DWORD(len(buffer))
        if not k.QueryFullProcessImageNameW(process,0,buffer,ctypes.byref(length)):return None
        if str(Path(buffer.value)).casefold()!=str(Path(expected_exe)).casefold():return None
    finally:k.CloseHandle(process)
    k.CreateToolhelp32Snapshot.argtypes=[wintypes.DWORD,wintypes.DWORD];k.CreateToolhelp32Snapshot.restype=wintypes.HANDLE
    k.Module32FirstW.argtypes=[wintypes.HANDLE,ctypes.POINTER(MODULEENTRY32W)];k.Module32FirstW.restype=wintypes.BOOL
    k.Module32NextW.argtypes=k.Module32FirstW.argtypes;k.Module32NextW.restype=wintypes.BOOL
    snapshot=k.CreateToolhelp32Snapshot(0x8|0x10,pid)
    if snapshot==ctypes.c_void_p(-1).value:return None
    try:
        entry=MODULEENTRY32W();entry.dwSize=ctypes.sizeof(entry);result=[]
        found=k.Module32FirstW(snapshot,ctypes.byref(entry))
        while found and len(result)<1024:
            result.append((entry.szModule,ctypes.cast(entry.modBaseAddr,ctypes.c_void_p).value,entry.modBaseSize,entry.szExePath))
            found=k.Module32NextW(snapshot,ctypes.byref(entry))
        return result
    finally:k.CloseHandle(snapshot)

def find_owned_process(expected_exe):
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes=[wintypes.DWORD,wintypes.DWORD];k.CreateToolhelp32Snapshot.restype=wintypes.HANDLE
    k.Process32FirstW.argtypes=[wintypes.HANDLE,ctypes.POINTER(PROCESSENTRY32W)];k.Process32FirstW.restype=wintypes.BOOL
    k.Process32NextW.argtypes=k.Process32FirstW.argtypes;k.Process32NextW.restype=wintypes.BOOL
    k.CloseHandle.argtypes=[wintypes.HANDLE];k.CloseHandle.restype=wintypes.BOOL
    snapshot=k.CreateToolhelp32Snapshot(0x2,0)
    if snapshot==ctypes.c_void_p(-1).value:return None
    try:
        entry=PROCESSENTRY32W();entry.dwSize=ctypes.sizeof(entry);candidates=[]
        found=k.Process32FirstW(snapshot,ctypes.byref(entry))
        while found:
            if entry.szExeFile.casefold()==Path(expected_exe).name.casefold():candidates.append(entry.th32ProcessID)
            found=k.Process32NextW(snapshot,ctypes.byref(entry))
    finally:k.CloseHandle(snapshot)
    for candidate in candidates:
        modules=owned_modules(candidate,expected_exe)
        if modules is not None:return candidate,modules
    return None

def severity(line,source):
    if re.search(r'\b(?:status|result|client_status)=(-?\d+)',line):
        values=re.findall(r'\b(?:status|result|client_status)=(-?\d+)',line)
        if any(int(value) not in (0,1) for value in values):return 'error'
    if any(word in line.lower() for word in ('[error]','[dz62_error]','native_error','can\'t compile','fatal','exception')):return 'error'
    if source=='DLL':return 'dll'
    if source=='NATIVO':return 'native'
    if '[DZ62_MOVE_ACK]' in line or '[DZ62_MOVE_CLIENT]' in line:return 'ok'
    return 'normal'

def main(role, runtime_root, loader_log_dir, expected_exe, profile_dir=None):
    import tkinter as tk
    from tkinter import ttk
    label='JOGO' if role=='client1' else 'SERVIDOR'
    root=tk.Tk();root.title('DEBUG '+label+' — DayZ 0.62');root.configure(bg='#101418')
    width=min(860,max(620,root.winfo_screenwidth()//2-60))
    left=30 if role=='server' else max(30,root.winfo_screenwidth()-width-30)
    root.geometry(f'{width}x500+{left}+50');root.minsize(600,300)
    header=tk.StringVar(value=f'DEBUG {label} | aguardando processo')
    tk.Label(root,textvariable=header,anchor='w',bg='#17212b',fg='#84c5ff',font=('Consolas',11,'bold'),padx=10,pady=8).pack(fill='x')
    toolbar=tk.Frame(root,bg='#101418');toolbar.pack(fill='x',padx=6,pady=4)
    paused=tk.BooleanVar();sticky=tk.BooleanVar();filter_text=tk.StringVar();channel=tk.StringVar(value='Todos')
    tk.Checkbutton(toolbar,text='Pausar tela',variable=paused,bg='#101418',fg='#cccccc',selectcolor='#17212b',activebackground='#101418').pack(side='left')
    tk.Checkbutton(toolbar,text='Sempre visível',variable=sticky,command=lambda:root.attributes('-topmost',sticky.get()),bg='#101418',fg='#cccccc',selectcolor='#17212b').pack(side='left')
    box=ttk.Combobox(toolbar,textvariable=channel,values=['Todos','Rotação','Erros','DLL / hooks'],state='readonly',width=13);box.pack(side='left',padx=5)
    tk.Entry(toolbar,textvariable=filter_text,bg='#17212b',fg='#eeeeee',insertbackground='white',width=18).pack(side='left')
    area=tk.Frame(root,bg='#050708');area.pack(fill='both',expand=True,padx=6,pady=4)
    text=tk.Text(area,bg='#050708',fg='#79b8ff',font=('Consolas',10),wrap='none',state='disabled',insertbackground='white',borderwidth=0)
    scroll=ttk.Scrollbar(area,command=text.yview);scroll.pack(side='right',fill='y');text.configure(yscrollcommand=scroll.set);text.pack(fill='both',expand=True)
    for name,color in [('normal','#79b8ff'),('native','#65d7c9'),('dll','#dfc57a'),('error','#ff6b6b'),('ok','#82d69b'),('historical','#8c99a7')]:text.tag_configure(name,foreground=color)
    state=tk.StringVar(value='Eventos instrumentados; horário de recebimento. DLLs: inventário observado a cada 2 s.')
    tk.Label(root,textvariable=state,anchor='w',bg='#101418',fg='#aab8c5',font=('Consolas',8),padx=8,pady=4).pack(fill='x')
    events=deque(maxlen=4000);displayed=0;pid=0;modules={};module_time=0;revision=0;rendered=None
    role_root=Path(runtime_root)/role
    expected=str(Path(expected_exe).resolve())
    profile=Path(profile_dir) if profile_dir else role_root/'trial_profile'
    rpt_name='DayZ_x64.RPT' if role=='client1' else 'DayZServerP69_x64.RPT'
    sources=[('SCRIPT',Tail(profile/'script.log')),('RPT',Tail(profile/rpt_name)),
      ('NATIVO',Tail(profile/'native_rotation.log')),('LOADER',Tail(Path(loader_log_dir)/(role+'_launcher.txt')))]
    def emit(source,line,historical=False):
        nonlocal revision
        stamp=time.strftime('%H:%M:%S')+f'.{int(time.time()%1*1000):03d}'
        events.append((source,line,historical,stamp))
        revision+=1
    for source,tail in sources:
        if source!='LOADER' and not tail.path.exists():
            emit('CONSOLE',f'FONTE AUSENTE: {source} esperado em {tail.path}')
        elif source!='LOADER':
            emit('CONSOLE',f'FONTE ATIVA: {source} lendo {tail.path}')
    def render():
        nonlocal displayed,rendered
        if paused.get():return
        key=(revision,filter_text.get(),channel.get())
        if rendered==key:return
        rendered=key
        needle=filter_text.get().casefold();selected=channel.get();rows=[]
        for source,line,historical,stamp in events:
            kind=severity(line,source)
            if needle and needle not in line.casefold():continue
            if selected=='Rotação' and not ('DZ62_' in line or source=='NATIVO'):continue
            if selected=='Erros' and kind!='error':continue
            if selected=='DLL / hooks' and not (source in ('DLL','LOADER') or ('HOOK' in line or 'INSTALL' in line)):continue
            prefix='histórico' if historical else 'recebido '+stamp
            rows.append((f'[{prefix}][{label}/{source}] {line}\n',kind))
        text.configure(state='normal');text.delete('1.0','end')
        for row,kind in rows[-2000:]:text.insert('end',row,kind)
        text.see('end');text.configure(state='disabled');displayed=len(rows)
    def save():
        path=HERE/(f'console_{role}_{time.strftime("%Y%m%d_%H%M%S")}.log')
        path.write_text(''.join(f'[{stamp}][{source}] {line}\n' for source,line,historical,stamp in events),encoding='utf-8');state.set('Salvo: '+str(path))
    def clear():
        nonlocal revision
        events.clear();revision+=1;render()
    tk.Button(toolbar,text='Limpar tela',command=clear).pack(side='right',padx=2)
    tk.Button(toolbar,text='Salvar',command=save).pack(side='right',padx=2)
    box.bind('<<ComboboxSelected>>',lambda event:render());filter_text.trace_add('write',lambda *args:render())
    def tick():
        nonlocal pid,modules,module_time
        try:
            for source,tail in sources:
                for line,historical in tail.read():emit(source,line,historical)
            receipt=Path(loader_log_dir)/(role+'_launch.json')
            if receipt.exists():
                info=json.loads(receipt.read_text(encoding='utf-8'));match=re.search(r'PID=(\d+)',info.get('stdout',''))
                newest=int(match.group(1)) if match else 0
                if newest!=pid:
                    pid=newest;modules={};module_time=0
                    emit('LOADER',f'PID={pid} DLL={info.get("dll","não registrada")} SHA256={info.get("dll_sha256","não registrada")}')
            if not pid and time.monotonic()-module_time>=2:
                module_time=time.monotonic();found=find_owned_process(expected)
                if found is not None:
                    pid,observed=found;modules={}
                    emit('PROCESS',f'PROCESSO OBSERVADO PID={pid} executável={expected}')
                    for name,address,size,path in observed:
                        modules[name.casefold()+':'+str(address)]=(name,address,size,path)
                        emit('DLL',f'MÓDULO OBSERVADO {name} base=0x{address:x} bytes={size} caminho={path}')
                    header.set(f'DEBUG {label} | PID {pid} | {len(modules)} módulos observados')
            if pid and time.monotonic()-module_time>=2:
                module_time=time.monotonic();observed=owned_modules(pid,expected)
                if observed is not None:
                    new={name.casefold()+':'+str(address):(name,address,size,path) for name,address,size,path in observed}
                    for key,entry in new.items():
                        if key not in modules:
                            name,address,size,path=entry;emit('DLL',f'MÓDULO OBSERVADO {name} base=0x{address:x} bytes={size} caminho={path}')
                    for key,entry in modules.items():
                        if key not in new:emit('DLL','MÓDULO NÃO PRESENTE NA NOVA AMOSTRA '+entry[0])
                    modules=new;header.set(f'DEBUG {label} | PID {pid} | {len(modules)} módulos observados')
                else:
                    emit('PROCESS',f'PROCESSO PID={pid} não está mais disponível; aguardando reabertura')
                    header.set(f'DEBUG {label} | aguardando processo');pid=0;modules={}
            render()
            (HERE/f'ready_{role}.json').write_text(json.dumps({'role':role,'console_visible':bool(root.winfo_viewable()),'title':root.title(),'game_pid':pid,'events_buffered':len(events),'displayed':displayed,'modules_observed':len(modules)},indent=2),encoding='utf-8')
        except Exception as error:
            emit('CONSOLE',f'Viewer: {type(error).__name__}: {error}')
        root.after(700,tick)
    emit('CONSOLE','Console independente iniciado. Fechar esta janela mantém o jogo e o servidor ativos.')
    root.after(100,tick);root.mainloop()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--role',choices=['client1','server'],required=True)
    parser.add_argument('--runtime-root',required=True,help='Pasta de logs auxiliares/recibos')
    parser.add_argument('--game-exe',required=True,help='Caminho completo do executável observado')
    parser.add_argument('--loader-log-dir',default=None,help='Pasta dos recibos e logs do launcher; padrão: runtime-root')
    parser.add_argument('--profile-dir',default=None,help='Pasta real que contém script.log e o RPT deste processo')
    args=parser.parse_args();loader_log_dir=args.loader_log_dir or args.runtime_root
    try:main(args.role,args.runtime_root,loader_log_dir,args.game_exe,args.profile_dir)
    except Exception:(HERE/f'error_{args.role}.log').write_text(traceback.format_exc(),encoding='utf-8');raise
