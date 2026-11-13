import csv,hashlib,math,re,statistics
from pathlib import Path
root=Path(__file__).resolve().parent
for line in (root/'SHA256SUMS').read_text().splitlines():
    digest,name=line.split('  ',1)
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
rows=list(csv.DictReader((root/'s5.csv').open()))
assert len(rows)==6
assert {(r['mode'],r['repeat']) for r in rows}=={(m,str(i)) for m in ['default','enlarged'] for i in [1,2,3]}
for row in rows:
    mode=row['mode'];rep=row['repeat'];d=root/f'raw/s5-sysv_mq-1024-1-gray-1-{rep}-{mode}'
    samples=list(csv.DictReader((d/'client-1.csv').open()))
    assert len(samples)==1000 and [int(r['seq']) for r in samples]==list(range(100,1100))
    assert all(r['client_id']=='1' and r['payload_bytes']=='3145728' and int(r['rtt_ns'])>0 for r in samples)
    assert 'matched=1000 mismatches=0 warmup=100' in (d/'client-1.log').read_text()
    assert 'responses=1000' in (d/'server.log').read_text()
    assert all(not p.read_bytes() for p in d.glob('*.err'))
    capacity=16384 if mode=='default' else 65536
    assert re.findall(r'msg_qbytes=(\d+)',(d/'channel.txt').read_text())==[str(capacity)]*2
    assert not re.search(r'^0x\S+\s+\d+\s+ce22b035\s',(d/'ipcs-after.txt').read_text(),re.M)
    resources=[next(csv.DictReader((d/name).open())) for name in ['client-1.csv.cpu.csv','server.cpu.csv']]
    rtt=sorted(int(r['rtt_ns']) for r in samples)
    metrics={f'p{p}_us':rtt[math.ceil(len(rtt)*p/100)-1]/1000 for p in [50,95,99]}
    metrics.update(max_us=max(rtt)/1000,mean_us=sum(rtt)/len(rtt)/1000)
    span=(int(resources[0]['last_recv_ns'])-int(resources[0]['first_send_ns']))/1e9
    assert span>0
    metrics.update(imgs=len(samples)/span,mbps=len(samples)*3145728/span/1e6)
    for metric in ['ru_utime_ms','ru_stime_ms','nvcsw','nivcsw','rss_start_kib','rss_end_kib']:
        metrics[metric]=sum(float(r[metric]) for r in resources)
    for metric,value in metrics.items():
        assert math.isclose(float(row[metric]),value,rel_tol=1e-9,abs_tol=0.000001),(mode,rep,metric,value,row[metric])
    print(f'{mode} repeat={rep}: 1000 samples, seq/ID/payload/logs, capacity={capacity}x2, independent metrics, own IPC cleanup OK')
metrics=['p50_us','p95_us','p99_us','max_us','mean_us','mbps','imgs','ru_utime_ms','ru_stime_ms','nvcsw','nivcsw','rss_start_kib','rss_end_kib']
medians=[]
for mode in ['default','enlarged']:
    medians.append(dict(mode=mode,msg_qbytes=16384 if mode=='default' else 65536,**{k:statistics.median(float(r[k]) for r in rows if r['mode']==mode) for k in metrics}))
with (root/'median-independent.csv').open('w') as f:
    w=csv.DictWriter(f,fieldnames=list(medians[0]));w.writeheader();w.writerows(medians)
print('Median values:',medians)
for metric in ['imgs','p99_us','ru_utime_ms','ru_stime_ms','nvcsw','rss_end_kib']:
    before,after=[r[metric] for r in medians]
    print(metric, f'{before:.6f} -> {after:.6f} ({(after/before-1)*100:+.2f}%)')
print('6 runs / 6000 samples: preservation hashes and independent recalculation OK; control/runtime I/O interpretation is documented in docs/diagnostics-measurement.md.')

import tarfile
project=Path(__file__).resolve().parents[3]
checked=0
with tarfile.open(root/'source.tar') as archive:
    for member in archive.getmembers():
        if member.isfile() and (member.name.startswith('src/') or member.name in ['Makefile','bench/aggregate.c','bench/inspect.c','bench/run_bench.sh','bench/demo.sh']):
            assert archive.extractfile(member).read()==(project/member.name).read_bytes(),member.name
            checked+=1
assert checked==45
assert (root/'environment.txt').read_text().splitlines()[-6:]==['kernel.msgmnb = 65536','kernel.msgmnb = 16384']*3
post=(root/'postcheck.txt').read_text()
assert 'kernel.msgmnb = 16384' in post
assert not re.search(r'^0x\S+\s+\d+\s+ce22b035\s',post,re.M)
print('Source/build/measurement tool snapshot: 45 files byte-equal; 3 restore pairs, final 16384 and own IPC cleanup OK')
