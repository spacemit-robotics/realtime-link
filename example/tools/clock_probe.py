#!/usr/bin/env python3
"""Map ground CLOCK_MONOTONIC into sky CLOCK_MONOTONIC; never set system clocks."""
import argparse,json,socket,struct,time
p=argparse.ArgumentParser()
p.add_argument('mode',choices=['server','client'])
p.add_argument('--host',required=True,help='server bind IP or client destination IP')
p.add_argument('--peer',default='10.0.91.82',help='only accept this client on server')
p.add_argument('--port',type=int,default=5099)
p.add_argument('--seconds',type=float,default=80)
p.add_argument('--out')
a=p.parse_args()
now=lambda:time.monotonic_ns()//1000
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
sock.settimeout(.25)
until=time.monotonic()+a.seconds
if a.mode=='server':
 sock.bind((a.host,a.port))
 while time.monotonic()<until:
  try: data,peer=sock.recvfrom(128)
  except socket.timeout: continue
  t2=now()
  if peer[0]!=a.peer or len(data)!=12 or data[:4]!=b'FPVT': continue
  t1=struct.unpack('!Q',data[4:])[0]
  sock.sendto(struct.pack('!4sQQQ',b'FPVT',t1,t2,now()),peer)
else:
 if not a.out: p.error('--out required for client')
 sock.connect((a.host,a.port))
 with open(a.out,'x',buffering=1) as out:
  while time.monotonic()<until:
   t1=now()
   sock.send(struct.pack('!4sQ',b'FPVT',t1))
   try: data=sock.recv(128)
   except (socket.timeout,ConnectionRefusedError): time.sleep(.05);continue
   t4=now()
   if len(data)!=28: continue
   magic,echo,t2,t3=struct.unpack('!4sQQQ',data)
   if magic!=b'FPVT' or echo!=t1: continue
   out.write(json.dumps(dict(t1=t1,t2=t2,t3=t3,t4=t4,
       ground_mid_us=(t1+t4)/2,offset_us=((t2-t1)+(t3-t4))/2,
       rtt_us=(t4-t1)-(t3-t2)))+'\n')
   time.sleep(.05)
sock.close()
