#include "engine.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace sf;
volatile uint64_t observable=0;
struct Measure{uint64_t n,wall,cpu,sum;};
template<class F>Measure measure(F&&f,uint64_t min_ns=80000000){
 uint64_t n=4096,sum=0;for(size_t i=0;i<4096;i++)sum+=f(i);observable=sum;
 for(;;){sum=0;auto cpu0=ns(CLOCK_THREAD_CPUTIME_ID),wall0=ns();for(uint64_t i=0;i<n;i++)sum+=f(i);auto wall=ns()-wall0,cpu=ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;observable=sum;if(wall>=min_ns)return{n,wall,cpu,sum};n=uint64_t(double(n)*min_ns/double(std::max(wall,uint64_t(1)))*1.1/4096+1)*4096;}
}
void micro(Config c,std::ostream&out,int repeat){
 pin(0);Corpus corpus(c.length,c.modules);StorageManager store(c.storage);RingMemory ring(c.handoff,c.length,c.modules,QUEUE);
 std::vector<std::unique_ptr<Session>>sessions;for(size_t i=0;i<ACTIVE;i++)sessions.push_back(std::make_unique<Session>(i,c.modules,c.handoff==Handoff::H6,c.handoff==Handoff::H4));
 for(auto&p:sessions){store.prepare(*p,p->index+1);store.update(*p,0,corpus);store.update(*p,1,corpus);}
 auto emit=[&](const char*stage,Measure v){out<<stage<<','<<repeat<<','<<v.n<<','<<v.wall<<','<<v.cpu<<','<<double(v.cpu)/v.n<<','<<double(v.wall)/v.n<<','<<v.sum<<'\n';out.flush();};
 auto assembly=measure([&](uint64_t i){auto&s=*sessions[i&(ACTIVE-1)];auto&h=ring.header(i);auto tmp=temp_bytes(s.id,1);build(h,ring.body(h),s,1,c,tmp);escape(&h);return h.mask+h.used;});emit("worker_assembly",assembly);
 if(c.handoff==Handoff::H6){auto descriptors=measure([&](uint64_t i){auto&s=*sessions[i&(ACTIVE-1)];auto&h=ring.header(i);auto tmp=temp_bytes(s.id,1);build(h,ring.body(h),s,1,c,tmp,false);escape(&h);return h.mask+h.used;});emit("worker_descriptor_only_frozen",descriptors);}
 // Stable per-phase immutable sources exist for every Logger repetition.
 for(size_t i=0;i<ACTIVE;i++){auto&s=*sessions[i];auto&h=ring.header(i);auto tmp=temp_bytes(s.id,1);build(h,ring.body(h),s,1,c,tmp);}
 Encoder encoder(c.length);
 auto logging=measure([&](uint64_t i){auto&h=ring.header(i);size_t n=encoder.encode(h,ring.body(h),c.handoff);return n+encoder.wire[0]+encoder.wire[n-1];});emit("logger_encode",logging);
 // Start local per-session lifecycle at phase zero. No queue or retirement
 // protocol is charged to this isolated Worker service-time experiment.
 for(auto&p:sessions){store.reset(*p);store.prepare(*p,p->index+1);}
 uint64_t generation=ACTIVE+1;
 auto worker=measure([&](uint64_t i){auto&s=*sessions[i&(ACTIVE-1)];unsigned phase=s.phase;store.update(s,phase,corpus);auto tmp=temp_bytes(s.id,phase);auto&h=ring.header(i);build(h,ring.body(h),s,phase,c,tmp);uint64_t witness=h.mask+h.used;escape(&h);if(phase==3){store.reset(s);store.prepare(s,generation++);}else s.phase=phase+1;return witness;});emit("worker_lifecycle_local",worker);
}
void write_pipeline(std::ostream&out,const RunStats&s,int rep){
 out<<"pipeline,"<<rep<<','<<s.offered<<','<<s.accepted<<','<<s.dropped<<','<<s.received<<','<<s.errors<<','<<s.wall<<','<<double(s.accepted)/s.wall<<','<<s.worker_cpu<<','<<s.logger_cpu<<','<<s.worker_spins<<','<<s.logger_spins<<','<<s.wire_bytes<<','<<s.checksum<<','<<s.reclaimed<<','<<s.hooked<<','<<s.immediate<<','<<s.retired_peak<<','<<s.free_low<<','<<s.pool_reserved<<','<<s.pool_growth<<','<<s.metadata<<','<<s.string_capacity<<','<<s.ring_bytes<<','<<s.return_bytes<<','<<s.maxrss<<'\n';out.flush();
}
Config parse_cfg(int argc,char**argv){Config c;if(argc>2)c.storage=Storage(std::stoi(argv[2]));if(argc>3)c.handoff=Handoff(std::stoi(argv[3]));if(argc>4)c.life=Life(std::stoi(argv[4]));if(argc>5)c.length=std::stoul(argv[5]);if(argc>6)c.modules=std::stoul(argv[6]);if(argc>7)c.sparse=std::stoi(argv[7]);if(argc>8)c.logger_cpu=std::stoi(argv[8]);if(argc>9)c.logger_zero=std::stoi(argv[9]);require(c.modules>=1&&c.modules<=8&&c.length<=512,"config out of bounds");return c;}
void consistency_demo(){
 Session s(0,3,true);StorageManager store(Storage::M2);Corpus corpus(64,3);store.prepare(s,7);store.update(s,0,corpus);store.update(s,1,corpus);
 Export snapshot=static_cast<const Export&>(s.modules[0]);const Export*live=&static_cast<const Export&>(s.modules[0]);uint64_t before=snapshot.value[1];store.update(s,2,corpus);
 require(snapshot.value[1]==before&&live->value[1]!=before,"consistency demo failed");
 std::cout<<"{\"test\":\"H6_live_pointer_is_not_snapshot\",\"expected_at_emit\":"<<before<<",\"H5_snapshot\":"<<snapshot.value[1]<<",\"H6_live_after_update\":"<<live->value[1]<<",\"concurrent_unsafe_code_executed\":false}\n";
}
int main(int argc,char**argv){try{
 require(argc>1,"mode required");std::string mode=argv[1];auto c=parse_cfg(argc,argv);std::cout<<std::setprecision(12);
 if(mode=="demo"){consistency_demo();return 0;}
 if(mode=="micro"){micro(c,std::cout,0);return 0;}
 if(mode=="smoke"||mode=="stress"||mode=="stress_lossless"){
  uint64_t n=mode!="smoke"?10000000:ACTIVE*12+19;bool drop=mode=="stress"&&c.life==Life::L3;Engine e(c);auto s=pipeline<true>(e,n,drop,true);write_pipeline(std::cout,s,0);return 0;
 }
 require(mode=="pipeline"||mode=="pipeline_once","unknown mode");uint64_t n=131072;
 {Engine calibration(c);auto s=pipeline<false>(calibration,n);n=uint64_t(double(n)*.36/std::max(s.wall,1e-6)/16384+1)*16384;}
 for(int rep=0;rep<(mode=="pipeline_once"?1:5);rep++){
  for(;;){Engine e(c);auto s=pipeline<false>(e,n);if(s.wall<.3){n=uint64_t(double(n)*.36/std::max(s.wall,1e-6)/16384+1)*16384;continue;}write_pipeline(std::cout,s,rep);break;}
 }
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
