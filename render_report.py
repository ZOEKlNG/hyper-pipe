from pathlib import Path
import sys, json, csv, base64, html, re
sys.path.insert(0,str(Path(__file__).resolve().parent/'pydeps'))
import markdown
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

ROOT=Path(__file__).resolve().parent;OUT=ROOT/'deliverables';FIG=OUT/'figures';FIG.mkdir(exist_ok=True)
e=json.loads((OUT/'evidence.json').read_text())
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,'axes.spines.right':False,'axes.titleweight':'bold','figure.facecolor':'white','axes.labelcolor':'#334155','text.color':'#0f172a','svg.fonttype':'none'})
colors=['#2563eb','#0d9488','#d97706','#a16207','#7c3aed']
def save(fig,name):
    fig.savefig(FIG/(name+'.svg'),bbox_inches='tight')
    fig.savefig(FIG/(name+'.png'),dpi=160,bbox_inches='tight')
    plt.close(fig)
def codec(c,n,op):
    return next(x for x in e['codec']+e['sbe'] if x['codec']==c and x['payload_bytes']==str(n) and x['pattern']=='random' and x['operation']==op)

ids=['fixed64_offset_checked','sbe_generated_validated','protobuf_varint_reuse','protobuf_fixed_reuse','flatbuffers_verified']
labels=['Fixed layout + checks','SBE generated + checks','Protobuf varint / reuse','Protobuf fixed / reuse','FlatBuffers + verifier']
fig,axs=plt.subplots(1,2,figsize=(12.3,4.0),sharey=True)
for ax,op,title in zip(axs,['encode','decode_all'],['Encode existing fields + tail','Decode all scalar fields + payload edges']):
    rr=[codec('flatbuffers_trusted' if c=='flatbuffers_verified' and op=='encode' else c,256,op) for c in ids]
    vals=np.array([float(r['ns_median']) for r in rr]);lo=vals-np.array([float(r['ns_min']) for r in rr]);hi=np.array([float(r['ns_max']) for r in rr])-vals
    ax.barh(np.arange(len(rr)),vals,color=colors,xerr=np.vstack([lo,hi]),capsize=3,alpha=.9)
    ax.set_xlabel('ns / record (median of 5 batch means)');ax.set_title(title,loc='left',pad=15)
    ax.set_yticks(np.arange(len(rr)),labels);ax.grid(axis='x',alpha=.18);ax.set_axisbelow(True)
    for y,v in enumerate(vals):ax.text(float(rr[y]['ns_max'])+max(vals)*.02,y,f'{v:.1f}',va='center')
    ax.set_xlim(0,max(float(r['ns_max']) for r in rr)*1.2)
axs[0].invert_yaxis();fig.suptitle('256 B tail; 4,096 changing events; hot reusable buffers',x=.05,ha='left',y=1.04,fontsize=13)
fig.tight_layout();save(fig,'codec')

rr=[x for x in e['transport'] if x['body_bytes']=='256'];labs={'tcp_record':'TCP record','tcp_batch32':'TCP app batch 32','udp_record':'UDP record','udp_mmsg32':'UDP mmsg 32','zmq_push_pull':'ZeroMQ PUSH/PULL'}
fig,axs=plt.subplots(1,2,figsize=(12.3,4))
v=np.array([float(x['median_delivered_mmsg_s']) for x in rr]);lo=v-np.array([float(x['min_delivered_mmsg_s']) for x in rr]);hi=np.array([float(x['max_delivered_mmsg_s']) for x in rr])-v
axs[0].barh([labs[x['mode']] for x in rr],v,xerr=np.vstack([lo,hi]),color=colors,capsize=3);axs[0].invert_yaxis();axs[0].set_xlabel('Delivered million records / second');axs[0].set_title('One-host loopback; 256 B application body',loc='left')
for y,x in enumerate(v):axs[0].text(x+.03,y,f'{x:.3f}',va='center')
axs[0].set_xlim(0,max(float(x['max_delivered_mmsg_s']) for x in rr)*1.14)
p=e['paced'];targets=np.array([float(x['target_msg_s']) for x in p])/1000;got=np.array([float(x['median_received_msg_s']) for x in p])/1000
xx=np.arange(len(p));axs[1].bar(xx-.18,targets,.36,label='Planned records',color='#cbd5e1');axs[1].bar(xx+.18,got,.36,label='Received records',color='#0d9488');axs[1].set_xticks(xx,[f'{int(v)}k/s' for v in targets]);axs[1].set_ylabel('Thousand records in a 1 second window');axs[1].set_title('Paced UDP: generator saturates at high targets',loc='left');axs[1].legend(frameon=False)
for ax in axs:ax.grid(axis='x' if ax is axs[0] else 'y',alpha=.18);ax.set_axisbelow(True)
fig.tight_layout();save(fig,'transport')

rr=[x for x in e['queue_simulation'] if x['consumer']=='ALL' and x['scenario'] in ['pause_50ms','hot_id_30pct']]
fig,axs=plt.subplots(1,2,figsize=(12.3,4.3))
for ax,scenario,title in zip(axs,['pause_50ms','hot_id_30pct'],['One consumer pauses for 50 ms','30% of records share one ID']):
    a=[x for x in rr if x['scenario']==scenario];x=[float(z['drop_pct']) for z in a];y=[float(z['wait_p99_us'])/1000 for z in a]
    ax.plot(x,y,'o-',color='#0d9488',markersize=8)
    for z,xx,yy in zip(a,x,y):ax.annotate(f"{int(z['capacity']):,} slots",(xx,yy),xytext=(6,9),textcoords='offset points')
    ax.set_title(title,loc='left');ax.set_xlabel('Dropped records (%)');ax.set_ylabel('Accepted-record waiting p99 (ms)');ax.grid(alpha=.18)
    ax.margins(x=.22,y=.22);ax.set_ylim(bottom=-max(y)*.07)
fig.suptitle('SIMULATION: 600k/s total input; 4 consumers x 200k/s; drop-new',x=.06,ha='left',fontsize=13,y=1.04);fig.tight_layout();save(fig,'queue_tradeoff')

fig,axs=plt.subplots(1,2,figsize=(12.3,4))
for deadline,label,col in [('0','Count only (32)','#d97706'),('10','32 or 10 us deadline','#2563eb')]:
    a=[x for x in e['batch_simulation'] if x['batch_max']=='32' and x['deadline_us']==deadline]
    axs[0].plot([int(x['rate_msg_s']) for x in a],[float(x['formation_p99_us']) for x in a],'o-',label=label,color=col)
axs[0].set_xscale('log');axs[0].set_yscale('log');axs[0].set_xlabel('Assumed Poisson arrivals / second');axs[0].set_ylabel('Batch formation p99 (us)');axs[0].set_title('SIMULATION: batch timer prevents low-rate stalls',loc='left');axs[0].legend(frameon=False);axs[0].grid(alpha=.18)
fl=e['flink'];groups=[('fixed','nokeyby'),('fixed','keyby'),('protobuf','nokeyby'),('protobuf','keyby')];vals=[];lows=[];highs=[]
import statistics
for c,m in groups:
    a=[float(x['data_window_mps']) for x in fl if x['codec']==c and x['mode']==m];v=statistics.median(a);vals.append(v);lows.append(v-min(a));highs.append(max(a)-v)
axs[1].bar(np.arange(4),vals,color=['#2563eb','#1d4ed8','#0d9488','#0f766e'],yerr=[lows,highs],capsize=3)
axs[1].set_xticks(np.arange(4),['Fixed\nmap','Fixed\nkeyBy+reduce','Protobuf\nmap','Protobuf\nkeyBy+reduce']);axs[1].set_ylabel('Million projected tuples / second');axs[1].set_title('Real Flink local MiniCluster: p=2, 30M records',loc='left');axs[1].grid(axis='y',alpha=.18);axs[1].set_axisbelow(True)
fig.tight_layout();save(fig,'batch_flink')

md=(OUT/'研究报告.md').read_text()
body=markdown.markdown(md,extensions=['tables','fenced_code','toc'])
def pic(name,caption):
    b64=base64.b64encode((FIG/(name+'.svg')).read_bytes()).decode()
    return f'<figure><img alt="{html.escape(caption)}" src="data:image/svg+xml;base64,{b64}"/><figcaption>{html.escape(caption)}</figcaption></figure>'
inserts={
 '<h2 id="3">':'codec',
 '<h2 id="6">':'transport',
 '<h2 id="8-':'queue_tradeoff',
 '<h2 id="10">':'batch_flink',
}
captions={'codec':'热缓存局部耗时；误差条是5次批均值的min/max，不是消息延迟分位。','transport':'左为吞吐容量；右显示计划生成速率与真实收到数量，避免把生成器目标当成已施加负载。','queue_tradeoff':'模拟假设，不是Flink/网卡能力；p99仅统计未丢弃消息。','batch_flink':'左为批形成模拟；右为真实本地Flink投影tuple实验，完整payload没有穿过shuffle。'}
for key,name in inserts.items():
    pos=body.find(key)
    if pos<0:raise RuntimeError('Chart insertion target missing '+key)
    body=body[:pos]+pic(name,captions[name])+body[pos:]
headings=re.findall(r'<h2 id="([^"]+)">(.*?)</h2>',body)
toc=''.join(f'<a href="#{i}">{title}</a>' for i,title in headings)
datasets={k:e[k] for k in ['codec','sbe','transport','rtt','paced','ipc','queue_simulation','batch_simulation','java','flink','component']}
dataset_json=json.dumps(datasets,ensure_ascii=False).replace('</','<\\/')
inspector='''<section id="inspector"><h2>检查汇总数据</h2><p>可在这里查看结果字段和筛选配置；完整逐轮原始数据随源码包提供。</p><div class="controls"><select id="dataset"></select><input id="filter" placeholder="筛选，例如 256、protobuf、pause_50ms"/><span id="count"></span></div><div class="scroll"><table id="data-table"></table></div></section>'''
css='''*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f4f7fa;color:#162338;font:16px/1.8 -apple-system,BlinkMacSystemFont,"Noto Sans CJK SC","Microsoft YaHei",sans-serif}a{color:#1761b8;text-decoration:none}a:hover{text-decoration:underline}header{background:#102339;color:white;padding:44px max(5vw,24px) 34px}header .tag{font-size:12px;letter-spacing:2px;color:#88d6ca}header h1{font-size:32px;line-height:1.4;max-width:1100px;margin:10px 0}header p{color:#b9cada;max-width:1050px;margin:12px 0}.layout{display:grid;grid-template-columns:235px minmax(0,1fr);max-width:1500px;margin:auto}nav{padding:28px 20px;position:sticky;top:0;align-self:start;max-height:100vh;overflow:auto;font-size:12px}nav a{display:block;padding:6px 0;color:#50637b}main{background:white;padding:36px 44px;min-width:0;border-left:1px solid #e4ebf2}main>h1{display:none}h2{margin-top:42px;padding-top:12px;border-top:1px solid #dce6ee;font-size:23px;line-height:1.5;color:#102f48;scroll-margin-top:18px}h3{font-size:18px;margin-top:32px}p{margin:16px 0}strong{color:#0d5262}ul,ol{padding-left:25px}li{margin:7px 0}table{border-collapse:collapse;width:100%;font-size:12px;line-height:1.6;margin:20px 0;display:block;overflow-x:auto}th{background:#eaf2f7;color:#25445c;white-space:nowrap}th,td{padding:9px 11px;text-align:left;border-bottom:1px solid #dce6ee}tr:nth-child(even)td{background:#f8fafc}pre{background:#102339;color:#deedf9;padding:20px;overflow:auto;border-radius:7px;font-size:12px;line-height:1.8}code{font:13px/1.6 ui-monospace,SFMono-Regular,Consolas,monospace;background:#edf3f8;padding:2px 4px;border-radius:3px}pre code{background:transparent;padding:0;color:inherit}figure{margin:30px 0;padding:15px;border:1px solid #dce6ee;border-radius:10px;background:#fff}figure img{width:100%;height:auto;display:block}figcaption{font-size:12px;color:#627287;padding:10px 4px 0}.controls{display:flex;gap:12px;align-items:center;flex-wrap:wrap}input,select{font:inherit;font-size:13px;padding:8px;border:1px solid #bdcedd;border-radius:5px}input{min-width:250px}.scroll{overflow:auto;max-height:460px}#data-table{font:11px/1.4 ui-monospace,monospace}footer{padding:25px;font-size:12px;color:#687c91} @media(max-width:900px){.layout{display:block}nav{display:none}main{padding:20px}header h1{font-size:24px}}@media print{body{background:white}nav,.controls,#inspector{display:none}.layout{display:block}header{background:white;color:#102339;padding:15px}header p,header .tag{color:#4c6177}main{border:0;padding:0}h2,figure{break-inside:avoid}a{color:inherit}}'''
script='''const datasets=DATASETS;const sel=document.getElementById('dataset'),q=document.getElementById('filter');for(const k of Object.keys(datasets)){const o=document.createElement('option');o.value=k;o.textContent=k;sel.appendChild(o)}function render(){const rows=datasets[sel.value],s=q.value.toLowerCase(),filtered=rows.filter(r=>JSON.stringify(r).toLowerCase().includes(s)),t=document.getElementById('data-table');t.replaceChildren();const keys=Object.keys(rows[0]);const head=t.createTHead().insertRow();keys.forEach(k=>{const th=document.createElement('th');th.textContent=k;head.appendChild(th)});const b=t.createTBody();filtered.slice(0,150).forEach(r=>{const tr=b.insertRow();keys.forEach(k=>{tr.insertCell().textContent=r[k]??''})});document.getElementById('count').textContent=filtered.length+' 条，最多显示150条'}sel.onchange=render;q.oninput=render;render();'''.replace('DATASETS',dataset_json)
script='(()=>{'+script+'})();'
page=f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>高性能通信组件：实测与架构研究</title><style>{css}</style></head><body><header><div class="tag">BENCHMARK · ARCHITECTURE · REPRODUCIBLE EVIDENCE</div><h1>高性能专用通信组件：从 C++ / DPDK 到 Java / Flink</h1><p>按负载选择编码、传输、队列和接入策略。包含本地真实测量、显式参数模拟与可运行原型；未测物理双机 DPDK / AF_XDP / RDMA。</p><p>2026-10-21 · 单机 KVM / AMD EPYC · 原始数据与源码另附</p></header><div class="layout"><nav>{toc}<a href="#inspector">检查汇总数据</a></nav><main>{body}{inspector}<footer>所有图表由随包数据生成。模拟和理论预算均单独标注；此报告可离线打开，无外部脚本或数据请求。</footer></main></div><script>{script}</script></body></html>'''
(OUT/'研究报告.html').write_text(page)
print('Rendered report and 4 standalone SVG/PNG figures')
