#!/usr/bin/env python3
"""Small QMP client; quit requires an already paused/shutdown VM."""
import argparse,json,socket
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--socket',required=True)
p.add_argument('command',choices=['status','stop','cont','quit','cpus','memory']);a=p.parse_args()
s=socket.socket(socket.AF_UNIX);s.settimeout(10);s.connect(a.socket);f=s.makefile('rwb',buffering=0)
if 'QMP' not in json.loads(f.readline()):raise SystemExit('Missing QMP greeting')
def call(command):
 f.write(json.dumps({'execute':command}).encode()+b'\n')
 while True:
  line=f.readline()
  if not line:raise RuntimeError('QMP disconnected before acknowledgement')
  result=json.loads(line)
  if 'error' in result:raise RuntimeError(result['error'])
  if 'return' in result:return result['return']
call('qmp_capabilities')
if a.command=='quit' and call('query-status').get('status') not in ('paused','shutdown','prelaunch'):
 raise SystemExit('VM is running; request stop and verify paused first')
commands={'status':'query-status','stop':'stop','cont':'cont','quit':'quit','cpus':'query-cpus-fast','memory':'query-memory-size-summary'}
r=call(commands[a.command])
if a.command=='stop':
 r=call('query-status')
 if r.get('status')!='paused':raise SystemExit('Stop unconfirmed: '+json.dumps(r))
print(json.dumps(r,indent=2));f.close();s.close()
