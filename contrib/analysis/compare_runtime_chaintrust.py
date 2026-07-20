#!/usr/bin/env python3
import argparse,csv,json,sqlite3,os

def main():
 ap=argparse.ArgumentParser(); ap.add_argument('--runtime',required=True); ap.add_argument('--independent',required=True); ap.add_argument('--summary',required=True); ap.add_argument('--diff',required=True); a=ap.parse_args()
 db=sqlite3.connect(a.summary+'.sqlite'); db.execute('create table runtime(hash text primary key, prev_hash text, height integer, block_trust text, chaintrust text, active integer)')
 with open(a.runtime,newline='') as f:
  r=csv.DictReader(f); batch=[]
  for x in r:
   batch.append((x['hash'],x['prev_hash'],int(x['height']),x['runtime_block_trust'],x['runtime_chaintrust'],int(x['is_active'])))
   if len(batch)>=10000: db.executemany('insert into runtime values(?,?,?,?,?,?)',batch); db.commit(); batch=[]
  if batch: db.executemany('insert into runtime values(?,?,?,?,?,?)',batch); db.commit()
 stats={'runtime_rows':db.execute('select count(*) from runtime').fetchone()[0],'independent_rows':0,'compared':0,'full_matches':0,'block_trust_mismatches':0,'chaintrust_mismatches':0,'height_mismatches':0,'prev_hash_mismatches':0,'active_mismatches':0,'missing_runtime':0,'missing_independent':0}
 with open(a.diff,'w',newline='') as df:
  w=csv.writer(df); w.writerow(['hash','height','field','runtime_value','independent_value','difference','prev_hash','is_active'])
  seen=set()
  with open(a.independent,newline='') as f:
   for x in csv.DictReader(f):
    stats['independent_rows']+=1; h=x['hash']; seen.add(h); y=db.execute('select prev_hash,height,block_trust,chaintrust,active from runtime where hash=?',(h,)).fetchone()
    if y is None: stats['missing_runtime']+=1; w.writerow([h,x['height'],'missing_runtime','','','','',x['is_active']]); continue
    stats['compared']+=1; dif=False; fields=[('prev_hash',y[0],x['prev_hash']),('height',y[1],int(x['height'])),('block_trust',y[2],x['independent_block_trust']),('chaintrust',y[3],x['independent_chaintrust']),('active',y[4],int(x['is_active']))]
    for field,rv,iv in fields:
     if rv!=iv:
      dif=True; key={'block_trust':'block_trust_mismatches','chaintrust':'chaintrust_mismatches','height':'height_mismatches','prev_hash':'prev_hash_mismatches','active':'active_mismatches'}[field]; stats[key]+=1; w.writerow([h,x['height'],field,rv,iv,'',x['prev_hash'],x['is_active']])
    if not dif: stats['full_matches']+=1
 stats['missing_independent']=stats['runtime_rows']-stats['compared']
 stats['status']='complete' if not any(stats[k] for k in ('missing_runtime','missing_independent','block_trust_mismatches','chaintrust_mismatches','height_mismatches','prev_hash_mismatches','active_mismatches')) else 'mismatch'
 with open(a.summary,'w') as f: json.dump(stats,f,indent=2); f.write('\n')
 print(json.dumps(stats,indent=2)); db.close(); os.unlink(a.summary+'.sqlite'); raise SystemExit(0 if stats['status']=='complete' else 1)
if __name__=='__main__': main()
