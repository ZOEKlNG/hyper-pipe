#!/usr/bin/env python3
"""Assumption-only deterministic queue and Poisson batch-formation simulations.
No values are hardware performance predictions. Python 3 + numpy required.
"""
import argparse,csv,json
from pathlib import Path
from collections import deque
import numpy as np

SEED=20261021

def quant(x,p):
    return float(np.percentile(x,p)) if len(x) else 0.0

def queue_scenario(name,capacity,seed=SEED):
    # 4 producers at 150k/s each; 4 consumers at 200k/s each.
    # Sorted union of staggered deterministic producers = total 600k/s.
    rate=600_000; duration=1.0; service=1/200_000
    n=int(rate*duration)
    arrivals=np.arange(n,dtype=np.float64)/rate
    if name=='burst_2x_20ms':
        extra=.4+np.arange(int(rate*.02),dtype=np.float64)/rate
        arrivals=np.sort(np.concatenate((arrivals,extra)),kind='stable')
    rng=np.random.default_rng(seed)
    ids=rng.integers(0,1<<40,len(arrivals),dtype=np.uint64)
    if name=='hot_id_30pct':
        ids[rng.random(len(ids))<.30]=0
    buckets=ids & np.uint64(4095)
    targets=(buckets%np.uint64(4)).astype(np.int32)
    queues=[deque() for _ in range(4)]
    next_free=[0.0]*4
    offered=[0]*4; accepted=[0]*4; dropped=[0]*4; maxq=[0]*4
    waits=[[] for _ in range(4)]; latencies=[[] for _ in range(4)]
    pause_s=.010 if name=='pause_10ms' else .050 if name=='pause_50ms' else 0
    ps=.4; pe=ps+pause_s
    for t,c in zip(arrivals,targets):
        c=int(c);t=float(t);q=queues[c];offered[c]+=1
        while q and q[0]<=t:
            q.popleft()
        if len(q)>=capacity:
            dropped[c]+=1
            continue
        start=max(t,next_free[c]);finish=start+service
        if c==0 and pause_s:
            if ps<=start<pe:
                start=pe;finish=start+service
            elif start<ps<finish:
                finish+=pause_s
        next_free[c]=finish;q.append(finish);accepted[c]+=1
        maxq[c]=max(maxq[c],len(q))
        waits[c].append(start-t);latencies[c].append(finish-t)
    rows=[]
    for c in range(4):
        assert offered[c]==accepted[c]+dropped[c]
        rows.append(dict(scenario=name,capacity=capacity,consumer=c,offered=offered[c],
          accepted=accepted[c],dropped=dropped[c],drop_pct=100*dropped[c]/offered[c],
          max_occupied_slots=maxq[c],wait_p50_us=quant(waits[c],50)*1e6,
          wait_p99_us=quant(waits[c],99)*1e6,latency_p99_us=quant(latencies[c],99)*1e6,
          last_finish_s=next_free[c],fully_drained_completed=accepted[c]))
    w=np.concatenate([np.asarray(z) for z in waits]);l=np.concatenate([np.asarray(z) for z in latencies])
    rows.append(dict(scenario=name,capacity=capacity,consumer='ALL',offered=sum(offered),
      accepted=sum(accepted),dropped=sum(dropped),drop_pct=100*sum(dropped)/sum(offered),
      max_occupied_slots=max(maxq),wait_p50_us=quant(w,50)*1e6,
      wait_p99_us=quant(w,99)*1e6,latency_p99_us=quant(l,99)*1e6,
      last_finish_s=max(next_free),fully_drained_completed=sum(accepted)))
    return rows

def batch_scenario(rate,batch,deadline_us):
    rng=np.random.default_rng(SEED+rate)
    n=100_000
    arr=np.cumsum(rng.exponential(1/rate,n))
    delays=[];sizes=[];pending=[];deadline=float('inf')
    def flush(t):
        if pending:
            delays.extend(t-x for x in pending);sizes.append(len(pending));pending.clear()
    for t in arr:
        t=float(t)
        if pending and t>=deadline:
            flush(deadline);deadline=float('inf')
        if not pending:
            deadline=t+deadline_us*1e-6 if deadline_us else float('inf')
        pending.append(t)
        if len(pending)>=batch:
            flush(t);deadline=float('inf')
    # Count-only final partial batch explicitly flushed at end of finite stream.
    # n divisible by tested batches, so this affects none of count-only rows.
    if pending:
        flush(deadline if deadline_us else float(arr[-1]))
    assert len(delays)==n and sum(sizes)==n
    return dict(rate_msg_s=rate,batch_max=batch,deadline_us=deadline_us,
      records=n,batches=len(sizes),mean_batch_size=float(np.mean(sizes)),
      formation_p50_us=quant(delays,50)*1e6,formation_p99_us=quant(delays,99)*1e6,
      formation_max_us=max(delays)*1e6)

def write(path,rows):
    with open(path,'w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',default='results');args=p.parse_args()
    out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
    rows=[]
    for name in ['uniform','hot_id_30pct','burst_2x_20ms','pause_10ms','pause_50ms']:
        for capacity in [256,4096,65536]:
            rows.extend(queue_scenario(name,capacity))
    write(out/'queue_simulation.csv',rows)
    rows=[batch_scenario(rate,b,d) for rate in [1000,100000,1000000]
      for b in [1,8,32] for d in [0,10]]
    write(out/'batch_simulation.csv',rows)
    # Expansion 4 -> 5: old modulo changes 80%; virtual-bucket remap changes only 20%.
    ids=np.arange(1_000_000,dtype=np.uint64)
    old=ids%4;new_mod=ids%5
    table=np.arange(4096)%4
    newtable=table.copy();newtable[:819]=4
    old_b=table[(ids%4096).astype(np.int64)];new_b=newtable[(ids%4096).astype(np.int64)]
    write(out/'route_simulation.csv',[
      dict(method='id_mod_consumers_4_to_5',ids=len(ids),moved=int(np.count_nonzero(old!=new_mod)),
        moved_pct=100*float(np.mean(old!=new_mod))),
      dict(method='4096_virtual_buckets_move_819',ids=len(ids),moved=int(np.count_nonzero(old_b!=new_b)),
        moved_pct=100*float(np.mean(old_b!=new_b)))])
    (out/'simulation_parameters.json').write_text(json.dumps(dict(seed=SEED,duration_s=1,
      producers=4,total_base_rate_msg_s=600000,consumers=4,consumer_capacity_msg_s=200000,
      queue_capacity_includes_in_service=True,queue_policy='drop_new',
      service='deterministic 5us per record',routing='id & 4095 -> bucket_owner, 4 consumers round robin',
      pauses='consumer0 only at 0.4s, 10 or 50ms; stops service including in-flight work',
      burst='2x total rate during [0.4,0.42)s',hotspot='30% records forced to id0; remaining uniform',
      queue_metrics='accepted records drained after arrivals stop; percentiles exclude dropped records',
      batch_model='Poisson arrivals, no processing/transport; batch formation only; final partial flushed',
      warning='SIMULATION ASSUMPTIONS, NOT MEASURED HARDWARE CAPACITY'),ensure_ascii=False,indent=2))
    print(f'wrote {out}')
if __name__=='__main__':main()
