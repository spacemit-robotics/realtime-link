#!/usr/bin/env python3
"""Join one uninterrupted, single-channel run by original UVC PTS."""
import argparse,bisect,collections,csv,json,math,statistics
p=argparse.ArgumentParser()
p.add_argument('--sky',required=True);p.add_argument('--ground',required=True)
p.add_argument('--clock',required=True);p.add_argument('--warmup',type=float,default=5)
p.add_argument('--out',required=True)
a=p.parse_args()
frames=collections.defaultdict(dict); counts=collections.Counter(); duplicates=set();flags=collections.Counter(); malformed=0
for path in (a.sky,a.ground):
 with open(path) as f:
  for row in csv.DictReader(f):
   try: pts=int(row['pts_us']); stamp=int(row['mono_us']); value=int(row['value'])
   except (ValueError,TypeError): malformed+=1;continue
   event=row['event'];counts[event]+=1
   if event in frames[pts]:
    # 既有日志契约：capture_ready 由采集适配器与媒体线程各记一次（前者是驱动出帧、
    # 后者是入队待编码），同一个 PTS 出现两次属正常，按文档规则取最早的一条。
    # 其他关键事件重复则说明该帧 trace 不可信，整帧排除。
    if event=='capture_ready':
     frames[pts][event]=min(frames[pts][event],stamp)
    else:
     duplicates.add(pts)
     frames[pts][event]=stamp
   else:
    frames[pts][event]=stamp
   if event=='gui_feedback_received': flags[int(row['value'])]+=1
with open(a.clock) as f: probes=[json.loads(line) for line in f if line.strip()]
if len(probes)<20: raise SystemExit('not enough valid clock probes')
# Minimum RTT per 10 s bucket reduces queueing bias. Interpolation assumes clock
# drift approximately linear between adjacent calibration anchors; half RTT is
# an asymmetry uncertainty, not a guarantee of hardware timestamp accuracy.
first=probes[0]['ground_mid_us']; buckets=collections.defaultdict(list)
for r in probes:
 if r['rtt_us']>=0: buckets[int((r['ground_mid_us']-first)//10_000_000)].append(r)
anchors=[min(v,key=lambda r:r['rtt_us']) for _,v in sorted(buckets.items())]
times=[r['ground_mid_us'] for r in anchors]
def offset(t):
 i=bisect.bisect_right(times,t)
 if i==0: r=anchors[0];return r['offset_us'],r['rtt_us']/2
 if i==len(anchors):r=anchors[-1];return r['offset_us'],r['rtt_us']/2
 l,r=anchors[i-1:i+1];w=(t-l['ground_mid_us'])/(r['ground_mid_us']-l['ground_mid_us'])
 return l['offset_us']*(1-w)+r['offset_us']*w,max(l['rtt_us'],r['rtt_us'])/2
required=['capture_ready','mjpeg_submit','mjpeg_done','encode_submit','encode_done','tx_first','tx_last','rx_first','rx_complete','decode_submit','decode_done','decode_callback','gui_begin','gui_submit','gui_return','gui_feedback_received']
start=min(f['capture_ready'] for f in frames.values() if 'capture_ready' in f)
rows=[];negative=[];mismatched=[];unbracketed=[]
for pts,f in sorted(frames.items()):
 if pts in duplicates or not all(k in f for k in required) or f['capture_ready']<start+a.warmup*1e6:continue
 if not (probes[0]['ground_mid_us'] <= f['rx_complete'] <= probes[-1]['ground_mid_us']):
  unbracketed.append(pts);continue
 off,err=offset(f['rx_complete']);offp,_=offset(f['gui_feedback_received'])
 d={'pts_us':pts,'clock_uncertainty_ms':err/1000,
 'capture_ms':(f['capture_ready']-pts)/1000,
 'preprocess_ms':(f['encode_submit']-f['capture_ready'])/1000,
 'mjpeg_decode_ms':(f['mjpeg_done']-f['mjpeg_submit'])/1000,
 'scale_ms':(f.get('scale_done',0)-f.get('scale_begin',0))/1000,
 'encode_ms':(f['encode_done']-f['encode_submit'])/1000,
 'tx_queue_ms':(f['tx_first']-f['encode_done'])/1000,
 'network_frame_ms':(f['rx_complete']+off-f['tx_first'])/1000,
 'tx_pacing_ms':(f['tx_last']-f['tx_first'])/1000,
 'rx_assembly_ms':(f['rx_complete']-f['rx_first'])/1000,
 'rx_queue_ms':(f['decode_submit']-f['rx_complete'])/1000,
 'decode_ms':(f['decode_done']-f['decode_submit'])/1000,
 'gui_submit_lower_ms':(f['gui_submit']-f['decode_done'])/1000,
 'gui_return_ms':(f['gui_return']-f['decode_done'])/1000,
 'gui_feedback_upper_ms':(f['gui_feedback_received']-f['decode_done'])/1000,
 'gui_dispatch_ms':(f['gui_begin']-f['decode_done'])/1000,
 'gui_upload_ms':(f['gui_submit']-f['gui_begin'])/1000,
 'gui_compositor_upper_ms':(f['gui_feedback_received']-f['gui_submit'])/1000,
 'capture_to_decode_ms':(f['decode_done']+off-pts)/1000,
 'end_to_end_upper_ms':(f['gui_feedback_received']+offp-pts)/1000}
 # Local negatives are invalid. Negative cross-host estimates are reported too,
 # never silently clamped into apparently plausible measurements.
 if any(v<0 for k,v in d.items() if k.endswith('_ms') and k!='clock_uncertainty_ms'):negative.append(d)
 rows.append(d)
if not rows:raise SystemExit('no fully matched frames; inspect trace/clock coverage')
keys=[k for k in rows[0] if k.endswith('_ms')]
def stats(vals):
 vals=sorted(vals);n=len(vals)
 return {'n':n,'mean':statistics.mean(vals),'p50':statistics.median(vals),'p95':vals[math.ceil(.95*n)-1],'max':vals[-1]}
valid_present=[(f['gui_presented']-f['decode_done'])/1000 for pts,f in frames.items() if 'gui_presented' in f and 'decode_done' in f and f.get('capture_ready',0)>=start+a.warmup*1e6]
result={'malformed_rows':malformed,'valid_gui_presented_stats_ms':stats(valid_present) if valid_present else None,'counts':dict(counts),'matched_frames':len(rows),'duplicate_pts_excluded':len(duplicates),'unbracketed_excluded':len(unbracketed),'negative_frames':len(negative),'warmup_seconds':a.warmup,'presentation_flags':dict(flags),'clock_anchors':anchors,'stats_ms':{k:stats([r[k] for r in rows]) for k in keys}}
with open(a.out+'.frames.csv','w') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
with open(a.out+'.json','w') as f:json.dump(result,f,indent=2)
print('events:',dict(counts))
print('matched',len(rows),'negative',len(negative),'duplicates',len(duplicates),'presentation_flags',dict(flags))
print('stage                         mean       p50       p95       max   (ms)')
for k,s in result['stats_ms'].items():print(f"{k:28s} {s['mean']:9.3f} {s['p50']:9.3f} {s['p95']:9.3f} {s['max']:9.3f}")
