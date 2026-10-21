#!/usr/bin/env python3
"""Real C++ -> Java loopback bounded-queue workload validation, not latency ranking."""
import csv,json,pathlib,subprocess,time
root=pathlib.Path(__file__).resolve().parent
out=root/'results';out.mkdir(exist_ok=True)
cases=[('low_20k',20000,1024,0),('normal_100k',100000,1024,0),('overload_100k_delay50us',100000,256,50)]
rows=[]
for case,rate,capacity,delay in cases:
 for rep in range(3):
  port=19100+len(rows)
  rx_cmd=['taskset','-c','2,3','java','-Xms128m','-Xmx256m','-cp',str(root/'build'),'UdpReceiver','--port',str(port),'--queue',str(capacity),'--seconds','4','--worker-delay-us',str(delay)]
  rx=subprocess.Popen(rx_cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  ready=rx.stdout.readline().strip()
  if not ready.startswith('READY'):raise RuntimeError(ready+rx.stderr.read())
  tx_cmd=['taskset','-c','1',str(root/'build/producer'),'--port',str(port),'--count',str(rate*2),'--payload','256','--batch','32','--flush-us','10','--rate',str(rate),'--producer','1','--session',str(1000+len(rows))]
  try:
   tx=subprocess.run(tx_cmd,capture_output=True,text=True,check=True,timeout=20)
   tail,err=rx.communicate(timeout=20)
   if rx.returncode:raise RuntimeError(tail+err)
  except BaseException:
   rx.kill();rx.communicate();raise
  txj=json.loads(tx.stdout.strip().splitlines()[-1]);rxj=json.loads(tail.strip().splitlines()[-1])
  assert txj['offered']==txj['kernel_accepted']+txj['local_send_drop']
  assert rxj['received']==rxj['invalid']+rxj['queued']+rxj['queue_drop']
  assert rxj['queued']==rxj['processed']
  if rxj['received']>txj['kernel_accepted']:raise AssertionError('unexpected duplicates')
  row={'case':case,'rep':rep,'target_rate':rate,'payload_bytes':256,'wire_bytes':328,'queue_capacity':capacity,'requested_worker_delay_us':delay,'offered':txj['offered'],'kernel_accepted':txj['kernel_accepted'],'local_send_drop':txj['local_send_drop'],'sender_elapsed_s':txj['elapsed_s'],'send_syscalls':txj['syscalls'],'java_received':rxj['received'],'java_invalid':rxj['invalid'],'java_queue_drop':rxj['queue_drop'],'java_processed':rxj['processed'],'unobserved_between_kernel_and_java':txj['kernel_accepted']-rxj['received'],'receiver_elapsed_s':rxj['elapsed_s'],'socket_rcvbuf':rxj['socket_rcvbuf']}
  rows.append(row)
  detail={'measurement':'loopback_cross_language_bounded_queue_validation','sender_command':tx_cmd,'receiver_command':rx_cmd,'ready':ready,'sender_stdout':tx.stdout,'sender_stderr':tx.stderr,'receiver_stdout':tail,'receiver_stderr':err,'row':row}
  (out/f'{case}-rep{rep}.json').write_text(json.dumps(detail,indent=2)+'\n')
  with (out/'workloads.csv').open('w',newline='') as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
  print(json.dumps(row),flush=True)
print('all workload counter conservation checks PASS',flush=True)
