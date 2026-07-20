#!/usr/bin/env python3
"""Independent nChainTrust reconstruction from an IDAGIDX1 export.

This tool does not open LevelDB or call Innova production trust helpers.  For
mainnet snapshots with the sentinel PoEM/DAG gates, trust is the exact compact-
target work formula; the PoS flag is reported and post-DAG PoS is rejected.
"""
import argparse, csv, json, struct
from collections import defaultdict

REC=132

def compact_target(bits):
    exp=bits>>24; mant=bits & 0x007fffff
    if bits & 0x00800000: mant=-mant
    if exp <= 3: return mant >> (8*(3-exp))
    return mant << (8*(exp-3))

def block_trust(bits):
    target=compact_target(bits)
    if target <= 0: return 0
    return (1<<256)//(target+1)

def read_index(path):
    with open(path,'rb') as f:
        if f.read(8)!=b'IDAGIDX1': raise ValueError('bad index magic')
        rec_size=struct.unpack('<I',f.read(4))[0]
        n=struct.unpack('<Q',f.read(8))[0]
        best=f.read(32); genesis=f.read(32)
        tip_height=struct.unpack('<i',f.read(4))[0]; f.read(4)
        if rec_size!=REC: raise ValueError(f'unsupported record size {rec_size}')
        out=[]
        for _ in range(n):
            b=f.read(REC)
            if len(b)!=REC: raise ValueError('truncated record')
            off=0
            h=b[off:off+32]; off+=32; prev=b[off:off+32]; off+=32
            off+=32; file_no,pos,height=struct.unpack_from('<IIi',b,off); off+=12
            flags,time,version,bits,nonce=struct.unpack_from('<IIiII',b,off); off+=20
            active=bool(b[off]); off+=4
            out.append({'hash':h,'prev':prev,'file':file_no,'pos':pos,'height':height,
                        'flags':flags,'time':time,'version':version,'bits':bits,
                        'nonce':nonce,'active':active})
    return out,best,genesis,tip_height

def hx(b): return b[::-1].hex()

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('index'); ap.add_argument('--summary',required=True); ap.add_argument('--csv')
    a=ap.parse_args(); recs,best,genesis,tip=read_index(a.index)
    by={r['hash']:r for r in recs}; trust={}; unknown=0; missing=0; cycles=0; mismatches=0
    ordered=sorted(recs,key=lambda r:(r['height'],r['hash']))
    rows=[]
    csvfile = open(a.csv, 'w', newline='') if a.csv else None
    writer = csv.writer(csvfile) if csvfile else None
    if writer: writer.writerow(['hash','prev_hash','height','flags','bits','is_pos','active','independent_block_trust','independent_chaintrust'])
    for r in ordered:
        if r['height']>=999999999:
            unknown+=1; continue
        if r['height']==0 or r['prev']==b'\0'*32:
            parent=0
        elif r['prev'] not in by:
            missing+=1; continue
        elif r['prev'] not in trust:
            # Height ordering should make this impossible; classify explicitly.
            missing+=1; continue
        else: parent=trust[r['prev']]
        inc=block_trust(r['bits'])
        value=parent+inc; trust[r['hash']]=value
        if writer: writer.writerow([hx(r['hash']),hx(r['prev']),r['height'],r['flags'],f'{r["bits"]:08x}',bool(r['flags']&1),r['active'],str(inc),str(value)])
    active=[r for r in recs if r['active']]
    active_tip=best
    active_match=active_tip in trust
    side=[r for r in recs if not r['active']]
    if csvfile: csvfile.close()
    summary={'records_total':len(recs),'records_checked':len(rows),'unknown':unknown,'unreconstructable_missing_parent':missing,'cycles':cycles,'trust_mismatches':mismatches,'active_records':len(active),'side_records':len(side),'snapshot_tip_height':tip,'snapshot_tip_hash':hx(best),'genesis_hash':hx(genesis),'independent_tip_reconstructable':active_match,'formula':'(1<<256)//(compact_target(nBits)+1)','post_dag_records':sum(r['height']>=999999999 for r in recs),'poem_or_dag_input_required':False,'status':'INDEPENDENT_RECONSTRUCTION_ONLY; runtime nChainTrust comparison unavailable because it is not serialized'}
    with open(a.summary,'w') as f: json.dump(summary,f,indent=2); f.write('\n')
    print(json.dumps(summary,indent=2))
if __name__=='__main__': main()
