// Supplementary workload; includes the frozen measurement helper unchanged.
#define main stateful_original_main
#include "../bench.cpp"
#undef main
int main(){try{
 pin(0);std::cout<<std::setprecision(12)<<"storage,repeat,stage,iterations,wall_ns,cpu_ns,cpu_ns_per_log,checksum,logical_session_bytes,session_capacity_bytes,pool_reserved_bytes\n";
 for(int repeat=0;repeat<5;repeat++)for(auto mode:{Storage::M1,Storage::M2,Storage::M3,Storage::A256,Storage::A512,Storage::A2048}){
  Config c;c.storage=mode;c.handoff=Handoff::H5;c.length=512;c.modules=3;
  Corpus corpus(512,3);std::array<size_t,6>lengths={12,32,64,128,256,512};
  for(size_t i=0;i<ACTIVE;i++){size_t f=0;for(size_t m=1;m<3;m++)for(size_t n=0;n<string_count(m);n++)corpus.data[(i*3+m)*STRINGS+n].resize(lengths[f++]);}
  StorageManager store(mode);RingMemory ring(c.handoff,512,3,QUEUE);std::vector<std::unique_ptr<Session>>sessions;
  for(size_t i=0;i<ACTIVE;i++){auto s=std::make_unique<Session>(i,3,false);store.prepare(*s,i+1);store.update(*s,0,corpus);store.update(*s,1,corpus);sessions.push_back(std::move(s));}
  size_t capacity=0;auto&first=*sessions.front();
  if(mode==Storage::M1||mode==Storage::M2){for(size_t m=1;m<3;m++)for(size_t f=0;f<string_count(m);f++)capacity+=first.owned[m*STRINGS+f].capacity();}
  else if(mode==Storage::M3)for(auto n:lengths)capacity+=StorageManager::slab_size(n);
  else for(auto [p,n]:first.blocks)capacity+=n;
  Encoder encoder(512);
  for(size_t i=0;i<ACTIVE;i++){auto&s=*sessions[i];auto&h=ring.header(i);auto tmp=temp_bytes(s.id,1);build(h,ring.body(h),s,1,c,tmp);auto n=encoder.encode(h,ring.body(h),c.handoff);require(encoder.verify(h,n,corpus),"mixed full payload validation");}
  auto emit=[&](const char*stage,Measure v){std::cout<<name(mode)<<','<<repeat<<','<<stage<<','<<v.n<<','<<v.wall<<','<<v.cpu<<','<<double(v.cpu)/v.n<<','<<v.sum<<",1004,"<<capacity<<','<<store.pool.reserved<<'\n';std::cout.flush();};
  auto logger=measure([&](uint64_t i){auto&h=ring.header(i);size_t n=encoder.encode(h,ring.body(h),c.handoff);return n+encoder.wire[0]+encoder.wire[n-1];});emit("logger_encode",logger);
  for(auto&p:sessions){store.reset(*p);store.prepare(*p,p->index+1);}uint64_t generation=ACTIVE+1;
  auto worker=measure([&](uint64_t i){auto&s=*sessions[i&(ACTIVE-1)];unsigned phase=s.phase;store.update(s,phase,corpus);auto tmp=temp_bytes(s.id,phase);auto&h=ring.header(i);build(h,ring.body(h),s,phase,c,tmp);auto witness=h.mask+h.used;escape(&h);if(phase==3){store.reset(s);store.prepare(s,generation++);}else s.phase++;return witness;});emit("worker_lifecycle_local",worker);
 }
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
