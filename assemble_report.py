from pathlib import Path
import csv, json, statistics, html, shutil, re, base64, zipfile, hashlib

ROOT=Path(__file__).resolve().parent
OUT=ROOT/'deliverables'
OUT.mkdir(exist_ok=True)

def read(path):
    return list(csv.DictReader(Path(path).open()))
def med(rows, key): return statistics.median(float(x[key]) for x in rows)
def fmt(x, digits=2): return f'{float(x):,.{digits}f}'
def table(headers,rows):
    return '\n'.join(['| '+' | '.join(headers)+' |','|'+'|'.join(['---']*len(headers))+'|']+['| '+' | '.join(str(c) for c in row)+' |' for row in rows])

codec=read(ROOT/'results/codec_summary.csv')
sbe=read(ROOT/'results/sbe_summary.csv')
codes=codec+sbe
def get(codec_name,n,op):
    return next(x for x in codes if x['codec']==codec_name and x['payload_bytes']==str(n) and x['pattern']=='random' and x['operation']==op)
names=[('固定布局，边界检查','fixed64_offset_checked'),('SBE，生成代码+校验','sbe_generated_validated'),('Protobuf varint，重用','protobuf_varint_reuse'),('Protobuf fixed，重用','protobuf_fixed_reuse'),('Protobuf varint，新建','protobuf_varint_fresh'),('FlatBuffers，验证器','flatbuffers_verified'),('JSON DOM+hex','json_nlohmann_hex')]
rows=[]
for label,c in names:
    dec=get(c,256,'decode_all'); rt=get(c,256,'roundtrip')
    enc=get('flatbuffers_trusted' if c=='flatbuffers_verified' else c,256,'encode')
    rows.append([label,fmt(enc['ns_median']),fmt(dec['ns_median']),fmt(rt['ns_median']),fmt(rt['wire_bytes_mean']),f"{fmt(rt['ns_min'])}–{fmt(rt['ns_max'])}"])
replacements={'codec_table':table(['编码','encode ns','decode_all ns','roundtrip ns','wire B','roundtrip运行范围 ns'],rows)}
fd=read(ROOT/'results/flat_direct_summary.csv')
fd256={x['operation']:x for x in fd if x['payload_bytes']=='256' and x['pattern']=='random'}
replacements['flat_direct_note']=f"256B尾部时，FlatBuffers保留builder缓冲的编码为 {fmt(fd256['encode_no_output_copy']['ns_median'])}ns，复制到string为 {fmt(fd256['encode_with_output_copy']['ns_median'])}ns。异步发送时必须延长builder缓冲的生命周期；不能刚提交就复用。"
pbE=float(get('protobuf_varint_reuse',256,'encode')['ns_median']);pbD=float(get('protobuf_varint_reuse',256,'decode_all')['ns_median'])
fxE=float(get('fixed64_offset_checked',256,'encode')['ns_median']);fxD=float(get('fixed64_offset_checked',256,'decode_all')['ns_median'])
replacements['cpu_budget_note']=f"以这组256B尾部为例，100万条/s时，Protobuf发送编码约{pbE/1000:.3f}核、接收解码约{pbD/1000:.3f}核；两端合计约{(pbE+pbD)/1000:.3f}核，固定布局约{(fxE+fxD)/1000:.3f}核。只是微基准的线性CPU预算，不含网络、队列、GC和Flink；真实数据可能完全不同。"

comp=read(ROOT/'results/compression_summary.csv')+read(ROOT/'results/compression_reuse_summary.csv')
rows=[]
for pat,plabel in [('random','随机'),('repetitive','重复日志')]:
    for c,label in [('memcpy','memcpy'),('lz4','LZ4'),('zstd_level1_reuse_context','Zstd1，复用context')]:
        a={x['operation']:x for x in comp if x['codec']==c and x['record_bytes']=='320' and x['batch']=='1' and x['pattern']==pat}
        main=a.get('compress',a.get('copy'))
        rows.append([plabel,label,fmt(main['ns_median']),fmt(a['decompress']['ns_median']) if 'decompress' in a else '—',fmt(main['output_bytes_batch_mean']),fmt(float(main['output_input_ratio'])*100)+'%'])
replacements['compression_table']=table(['内容','操作/格式','复制或压缩 ns','解压 ns','输出B','输出/输入'],rows)

ipc=read(ROOT/'ipc_sim/results/ipc_summary.csv')
rows=[]
for x in ipc:
    if x['message_bytes']=='256':
        rows.append(['线程' if x['model']=='threads' else 'fork+mmap','原地构造' if x['construction']=='inplace_pool' else 'scratch+copy',x['batch'],fmt(x['median_Mmsg_s'],3),f"{fmt(x['min_Mmsg_s'],3)}–{fmt(x['max_Mmsg_s'],3)}"])
replacements['ipc_table']=table(['模型','构造','batch','Mmsg/s 中位数','运行范围'],rows)

tp=read(ROOT/'transport/results_throughput_summary.csv');rt=read(ROOT/'transport/results_rtt_summary.csv');pc=read(ROOT/'transport/results_paced_summary.csv')
labels={'tcp_record':'TCP单条','tcp_batch32':'TCP应用批32','udp_record':'UDP单条','udp_mmsg32':'UDP mmsg32','zmq_push_pull':'ZeroMQ PUSH/PULL'}
replacements['transport_table']=table(['后端','交付Mmsg/s','范围','进程CPU核约值','missing'],[[labels[x['mode']],fmt(x['median_delivered_mmsg_s'],3),f"{fmt(x['min_delivered_mmsg_s'],3)}–{fmt(x['max_delivered_mmsg_s'],3)}",fmt(x['median_process_cpu_cores']),x['total_missing']] for x in tp if x['body_bytes']=='256'])
replacements['rtt_table']=table(['轻载往返','p50 µs','p99 µs','最差一轮p99 µs'],[[x['mode'],fmt(x['median_repeat_p50_us']),fmt(x['median_repeat_p99_us']),fmt(x['worst_repeat_p99_us'])] for x in rt if x['body_bytes']=='256'])
replacements['paced_table']=table(['计划条/s','实际收到条/s','生成器未执行%','已accepted后缺失%','计划→收到p99 µs','发送落后p99 µs','实际send→收到p99 µs'],[[fmt(x['target_msg_s'],0),fmt(x['median_received_msg_s'],0),fmt(x['median_schedule_missed_pct']),fmt(x['median_accepted_missing_pct']),fmt(x['median_arrival_minus_planned_p99_us']),fmt(x['median_sender_lateness_p99_us']),fmt(x['median_actual_send_to_arrival_p99_us'])] for x in pc])+'\n\n高档位主要撞到发送生成器上限：其追赶计划时刻产生数百毫秒调度欠债，**没有真的把50万/100万条每秒注入内核**。成功socket接受到接收的p99仍为几十微秒；不能把计划→接收的全部延迟归因于网络，也不能用accepted后零缺失证明该目标负载无丢失。生产可丢失组件更适合按最大年龄跳过陈旧输入，单列source-side discard。'

qs=read(ROOT/'ipc_sim/results/queue_simulation.csv')
replacements['queue_table']=table(['模拟场景','每consumer容量','总体drop%','已接受等待p99 ms','最晚完成时刻 s'],[[{'uniform':'均匀','hot_id_30pct':'30%同一id','pause_50ms':'一consumer暂停50ms'}[x['scenario']],x['capacity'],fmt(x['drop_pct'],3),fmt(float(x['wait_p99_us'])/1000,3),fmt(x['last_finish_s'],3)] for x in qs if x['consumer']=='ALL' and x['scenario'] in ['uniform','hot_id_30pct','pause_50ms']])
bs=read(ROOT/'ipc_sim/results/batch_simulation.csv')
a=next(x for x in bs if x['rate_msg_s']=='1000' and x['batch_max']=='32' and x['deadline_us']=='0');b=next(x for x in bs if x['rate_msg_s']=='1000' and x['batch_max']=='32' and x['deadline_us']=='10')
replacements['batch_note']=f"Poisson1000条/s、batch32的模拟中，只等数量的形成等待p99为{float(a['formation_p99_us'])/1000:.2f}ms；加10µs deadline后p99为{float(b['formation_p99_us']):.2f}µs，平均batch只有{float(b['mean_batch_size']):.2f}条。这是精确timer假设下的形成等待，不是Linux会保证的真实发送时延。"
rts=read(ROOT/'ipc_sim/results/route_simulation.csv')
replacements['route_note']=f"在100万个连续id的扩容模拟中，id%N从4变5移动{float(rts[0]['moved_pct']):.2f}%的id；4096虚拟桶只迁移819个时约{float(rts[1]['moved_pct']):.2f}%。这只是给定分布下的映射比例，热点桶应按流量加权，不代表迁移耗时或有序保证。"

jv=read(ROOT/'java_flink/results/java_codec.csv')
rows=[]
for n in [0,64,256,1024]:
    for mode,label in [('heap','固定布局heap'),('direct','固定布局direct'),('protobuf','Protobuf parseFrom')]:
        rr=[x for x in jv if x['payload_bytes']==str(n) and x['mode']==mode]
        vals=[float(x['ns_per_op']) for x in rr]
        rows.append([n,label,fmt(statistics.median(vals)),f'{fmt(min(vals))}–{fmt(max(vals))}'])
replacements['java_table']=table(['尾部B','Java读取模式','ns/消息 中位','5轮范围'],rows)
fpath=ROOT/'java_flink/results/flink_long.csv'
if not fpath.exists(): raise RuntimeError('Long Flink verification not ready')
fl=read(fpath);rows=[]
for c in ['fixed','protobuf']:
    for mode in ['nokeyby','keyby']:
        rr=[x for x in fl if x['codec']==c and x['mode']==mode]
        if len(rr)!=3: raise RuntimeError('Incomplete long Flink results')
        rows.append([c,mode,rr[0]['parallelism'],fmt(rr[0]['records'],0),fmt(med(rr,'wall_ms')),fmt(med(rr,'data_window_ms')),fmt(med(rr,'data_window_mps')),f"{fmt(min(float(x['data_window_mps']) for x in rr))}–{fmt(max(float(x['data_window_mps']) for x in rr))}"])
replacements['flink_table']=table(['codec','路径','p','records/轮','job wall ms','data window ms','Mrec/s','3轮范围'],rows)

cpath=ROOT/'component/results/workloads.csv'
if not cpath.exists(): raise RuntimeError('Component validation not ready')
cr=read(cpath)
# Scenario field names are mapped only after inspecting the final component CSV.
scenario_key='case'
if scenario_key not in cr[0]: raise RuntimeError('Need component CSV schema mapping: '+str(cr[0]))
rows=[]
for name in dict.fromkeys(x[scenario_key] for x in cr):
    rr=[x for x in cr if x[scenario_key]==name]
    rows.append([name]+[fmt(med(rr,k),0) for k in ['offered','kernel_accepted','java_received','java_queue_drop','java_processed']])
replacements['component_table']=table(['真实跨语言场景，3轮中位计数','offered','kernel accepted','Java received','queue drop','processed'],rows)

rows=[]
for app in [128,320,1088]:
    wire=app+66
    rows.append([app,wire]+[fmt(g*1e9/(wire*8)/1e6,3) for g in [10,25,100]])
replacements['wire_budget']=table(['应用datagram B','线缆占用B（近似）','10GbE理论Mmsg/s','25GbE理论Mmsg/s','100GbE理论Mmsg/s'],rows)+'\n\n理论模型：每消息一UDP/IPv4包，无VLAN/隧道，14B Ethernet+20B IPv4+8B UDP+4B FCS+8B preamble/SFD+12B IFG，总附加66B，消息足够长无需最小帧padding；没有算交换机/PCIe/CPU开销。320B应用消息在1000万条/s时约30.88Gb/s，因此即使CPU无限快也超过单25GbE的该模型上限。聚合多条/包可减少每条头开销，另需预算批量等待与一个丢包影响多条日志。'

text=(ROOT/'report_template.md').read_text()
for k,v in replacements.items(): text=text.replace('{{'+k+'}}',v)
if '{{' in text: raise RuntimeError('Unfilled report placeholder')
(OUT/'研究报告.md').write_text(text)
(OUT/'evidence.json').write_text(json.dumps({'environment':json.loads((ROOT/'results/environment.json').read_text()),'codec':codec,'sbe':sbe,'transport':tp,'rtt':rt,'paced':pc,'ipc':ipc,'queue_simulation':qs,'batch_simulation':bs,'java':jv,'flink':fl,'component':cr},ensure_ascii=False,indent=2))
print('Report markdown and evidence assembled')
