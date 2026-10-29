#include "common.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <sched.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <immintrin.h>
using namespace handoff;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
uint64_t ns(clockid_t clock=CLOCK_MONOTONIC){timespec t;clock_gettime(clock,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
void pin(int cpu){cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof(set),&set))throw std::runtime_error("affinity failure");}
struct alignas(128) Counter{std::atomic<uint64_t>v{0};};
struct Queue{Counter head,tail,ready,go;};
struct alignas(64) Slot{
 alignas(64) std::array<uint8_t,MAX_WIRE>storage;
 uint64_t sequence=0;uint32_t corpus_index=0,wire_len=0;
 NativeSlot&native(){return *std::launder(reinterpret_cast<NativeSlot*>(storage.data()));}
};
static_assert(sizeof(NativeSlot)<=MAX_WIRE);
enum class Mode{A,A_scratch,B,B_direct,D_pb_object};
const char*mode_name(Mode m){switch(m){case Mode::A:return"A_native_slot";case Mode::A_scratch:return"A_scratch_then_slot";case Mode::B:return"B_worker_pb_batch_copy";case Mode::B_direct:return"B_direct_worker_pb";case Mode::D_pb_object:return"D_shared_pb_object";}return"invalid";}
struct Stats{uint64_t worker_cpu_ns=0,logger_cpu_ns=0,worker_polls=0,logger_polls=0,worker_stalls=0,logger_stalls=0,checksum=0,wire_bytes=0,errors=0,batch_flushes=0,object_space_bytes=0;double elapsed_s=0;};
struct Config{Mode mode;Shape shape;size_t tail_bytes,batch,capacity;uint64_t messages=0;};

template<bool Verify> Stats run_once(const Config&cfg,const std::vector<Packet>&corpus,uint64_t messages){
 Queue q;std::vector<Slot>slots(cfg.capacity);
 if(cfg.mode==Mode::A||cfg.mode==Mode::A_scratch)for(auto&s:slots)new(s.storage.data())NativeSlot;
 std::vector<std::unique_ptr<PBState>>objects;
 Stats stats;
 if(cfg.mode==Mode::D_pb_object){
  objects.reserve(cfg.capacity);
  for(size_t i=0;i<cfg.capacity;i++){objects.push_back(std::make_unique<PBState>());fill_pb(*objects.back(),cfg.shape,extract(corpus[i&(CORPUS_N-1)]));stats.object_space_bytes+=objects.back()->flat.SpaceUsedLong()+objects.back()->nested.SpaceUsedLong();}
 }
 PBState worker_pb;NativeSlot scratch;
 // Prewarm Worker PB strings and retained oneof child outside timing.
 fill_pb(worker_pb,cfg.shape,extract(corpus.back()));
 auto logger=[&]{
  pin(1);PBState logger_pb;fill_pb(logger_pb,cfg.shape,extract(corpus.back()));
  std::array<uint8_t,65536>final_frame;size_t used=0;uint64_t tail=0,head=0,checksum=0,wirebytes=0,errors=0,spins=0,stalls=0,flushes=0;
  q.ready.v.store(1,std::memory_order_release);while(!q.go.v.load(std::memory_order_acquire))_mm_pause();
  uint64_t cpu0=ns(CLOCK_THREAD_CPUTIME_ID);
  while(tail<messages){
   if(head==tail){head=q.head.v.load(std::memory_order_acquire);if(head==tail){++stalls;do{++spins;_mm_pause();head=q.head.v.load(std::memory_order_acquire);}while(head==tail);}}
   size_t take=std::min(uint64_t(cfg.batch),head-tail);
   for(size_t j=0;j<take;j++){
    auto&slot=slots[(tail+j)&(cfg.capacity-1)];if(slot.sequence!=tail+j)++errors;
    const uint8_t*wire=nullptr;size_t n=0;
    if(cfg.mode==Mode::B_direct){wire=slot.storage.data();n=slot.wire_len;}
    else{
     if(final_frame.size()-used<MAX_WIRE+4){used=0;++flushes;}
     auto*out=final_frame.data()+used+4;
     if(cfg.mode==Mode::A||cfg.mode==Mode::A_scratch)n=encode_pb(logger_pb,cfg.shape,view_slot(slot.native()),out,MAX_WIRE);
     else if(cfg.mode==Mode::D_pb_object)n=serialize_pb(*objects[(tail+j)&(cfg.capacity-1)],cfg.shape,out,MAX_WIRE);
     else{n=slot.wire_len;std::memcpy(out,slot.storage.data(),n);}
     uint32_t length=uint32_t(n);std::memcpy(final_frame.data()+used,&length,4);wire=out;used+=n+4;escape(final_frame.data());
    }
    if(n>MAX_WIRE){++errors;n=0;}
    if constexpr(Verify){if(!verify_wire(cfg.shape,std::string_view(reinterpret_cast<const char*>(wire),n),extract(corpus[slot.corpus_index])))++errors;}
    checksum+=wire_checksum(wire,n);wirebytes+=n;escape(wire);
   }
   tail+=take;q.tail.v.store(tail,std::memory_order_release);
  }
  stats.logger_cpu_ns=ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;stats.logger_polls=spins;stats.logger_stalls=stalls;stats.checksum=checksum;stats.wire_bytes=wirebytes;stats.errors=errors;stats.batch_flushes=flushes+(used?1:0);
 };
 std::thread consumer(logger);pin(0);while(!q.ready.v.load(std::memory_order_acquire))_mm_pause();
 uint64_t head=0,tail=0,spins=0,stalls=0,cpu0=ns(CLOCK_THREAD_CPUTIME_ID),wall0=ns();q.go.v.store(1,std::memory_order_release);
 while(head<messages){
  size_t take=std::min(uint64_t(cfg.batch),messages-head);
  if(head+take-tail>cfg.capacity){tail=q.tail.v.load(std::memory_order_acquire);if(head+take-tail>cfg.capacity){++stalls;do{++spins;_mm_pause();tail=q.tail.v.load(std::memory_order_acquire);}while(head+take-tail>cfg.capacity);}}
  for(size_t j=0;j<take;j++){
   uint64_t seq=head+j;size_t index=seq&(cfg.capacity-1),ci=seq&(CORPUS_N-1);auto&slot=slots[index];SourceView source=extract(corpus[ci]);
   if(cfg.mode==Mode::A)fill_slot(slot.native(),source);
   else if(cfg.mode==Mode::A_scratch){fill_slot(scratch,source);std::memcpy(&slot.native(),&scratch,offsetof(NativeSlot,tail)+source.flat.size);}
   else if(cfg.mode==Mode::D_pb_object)fill_pb(*objects[index],cfg.shape,source);
   else slot.wire_len=uint32_t(encode_pb(worker_pb,cfg.shape,source,slot.storage.data(),MAX_WIRE));
   slot.sequence=seq;slot.corpus_index=uint32_t(ci);
  }
  head+=take;q.head.v.store(head,std::memory_order_release);
 }
 // This final drain spin belongs to Worker total thread CPU; it is counted.
 while(q.tail.v.load(std::memory_order_acquire)<messages){++spins;_mm_pause();}
 stats.elapsed_s=double(ns()-wall0)/1e9;stats.worker_cpu_ns=ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;stats.worker_polls=spins;stats.worker_stalls=stalls;consumer.join();
 if(stats.errors)throw std::runtime_error("handoff correctness errors");return stats;
}

int main(int argc,char**argv){try{
 bool smoke=false;int repeats=5;double min_seconds=.3;std::string path="handoff_results.csv";
 for(int a=1;a<argc;a++){std::string x=argv[a];if(x=="--smoke")smoke=true;else if(x=="--output"&&a+1<argc)path=argv[++a];else if(x=="--repeats"&&a+1<argc)repeats=std::stoi(argv[++a]);else if(x=="--min-seconds"&&a+1<argc)min_seconds=std::stod(argv[++a]);else throw std::invalid_argument("unknown argument "+x);}
 if(!smoke&&min_seconds<.3)throw std::invalid_argument("performance minimum is 0.3 sec");
 alarm(900);std::vector<Config>configs;
 for(Shape shape:{Shape::Flat,Shape::Nested})for(size_t bytes:{64,256,1024})for(size_t batch:{1,32})for(Mode mode:{Mode::A,Mode::A_scratch,Mode::B,Mode::B_direct,Mode::D_pb_object})configs.push_back({mode,shape,bytes,batch,256});
 for(Shape shape:{Shape::Flat,Shape::Nested})for(size_t batch:{1,32})for(Mode mode:{Mode::A,Mode::A_scratch,Mode::B,Mode::B_direct,Mode::D_pb_object})configs.push_back({mode,shape,256,batch,4096});
 std::array<std::vector<Packet>,3>corpora={make_corpus(64),make_corpus(256),make_corpus(1024)};
 auto corpus_for=[&](size_t n)->const std::vector<Packet>&{return corpora[n==64?0:n==256?1:2];};
 if(smoke){
  uint64_t transition_checks=0;
  for(Shape shape:{Shape::Flat,Shape::Nested}){
   PBState state;NativeSlot native;std::array<uint8_t,MAX_WIRE>wire;const auto* retained_http=&state.nested.http();
   SourceView empty;empty.flat={"",0};for(auto&part:empty.parts)part={"",0};
   SourceView high=extract(corpora[2][0]);high.h.id=UINT64_MAX;high.h.seq=0;high.h.timestamp=UINT64_MAX;high.h.value0=0;high.h.value1=UINT64_MAX;high.h.producer=UINT32_MAX;high.h.stream=0;high.h.type=UINT32_MAX;high.h.flags=0;
   for(const auto&source:std::vector<SourceView>{extract(corpora[0][1]),empty,high,empty,extract(corpora[1][3])}){
    fill_slot(native,source);auto n=encode_pb(state,shape,view_slot(native),wire.data());
    if(!verify_wire(shape,std::string_view(reinterpret_cast<const char*>(wire.data()),n),source))throw std::runtime_error("reuse transition failure");
    if(&state.nested.http()!=retained_http)throw std::runtime_error("oneof child unexpectedly replaced");++transition_checks;
   }
  }
  uint64_t checks=0;for(const auto&cfg:configs){auto stats=run_once<true>(cfg,corpus_for(cfg.tail_bytes),10003);if(stats.errors)throw std::runtime_error("smoke mismatch");checks+=10003;}std::cout<<"PASS configs="<<configs.size()<<" complete_field_payload_checks="<<checks<<" reuse_transition_checks="<<transition_checks<<"\n";return 0;}
 // Separate calibration; main repetitions are shuffled across all configurations.
 for(auto&cfg:configs){auto r=run_once<false>(cfg,corpus_for(cfg.tail_bytes),50000);cfg.messages=std::max(uint64_t(100000),uint64_t(std::ceil(50000*min_seconds*1.12/r.elapsed_s/32))*32);}
 std::ofstream out(path);if(!out)throw std::runtime_error("cannot write results");out<<"mode,schema,tail_bytes,batch,queue_slots,slot_bytes,repeat,messages,elapsed_s,msg_per_s,worker_total_cpu_ns,logger_total_cpu_ns,worker_total_cpu_ns_per_msg,logger_total_cpu_ns_per_msg,worker_spin_polls,logger_spin_polls,worker_stall_episodes,logger_stall_episodes,wire_bytes_mean,checksum,errors,dropped,sink_batch_flushes,ring_allocated_bytes,pb_object_space_bytes,order_index\n";out<<std::setprecision(12);
 std::vector<size_t>order(configs.size());std::iota(order.begin(),order.end(),0);std::mt19937 rng(20261029);size_t oi=0;
 for(int rep=0;rep<repeats;rep++){
  std::shuffle(order.begin(),order.end(),rng);
  for(size_t ix:order){auto&cfg=configs[ix];Stats r;
   for(;;){r=run_once<false>(cfg,corpus_for(cfg.tail_bytes),cfg.messages);if(r.elapsed_s>=min_seconds)break;cfg.messages=uint64_t(std::ceil(cfg.messages*min_seconds*1.12/r.elapsed_s/32))*32;}
   out<<mode_name(cfg.mode)<<','<<shape_name(cfg.shape)<<','<<cfg.tail_bytes<<','<<cfg.batch<<','<<cfg.capacity<<','<<sizeof(Slot)<<','<<rep<<','<<cfg.messages<<','<<r.elapsed_s<<','<<cfg.messages/r.elapsed_s<<','<<r.worker_cpu_ns<<','<<r.logger_cpu_ns<<','<<double(r.worker_cpu_ns)/cfg.messages<<','<<double(r.logger_cpu_ns)/cfg.messages<<','<<r.worker_polls<<','<<r.logger_polls<<','<<r.worker_stalls<<','<<r.logger_stalls<<','<<double(r.wire_bytes)/cfg.messages<<','<<r.checksum<<','<<r.errors<<",0,"<<r.batch_flushes<<','<<sizeof(Slot)*cfg.capacity<<','<<r.object_space_bytes<<','<<oi++<<'\n';out.flush();
   // Adapt next round's count, while every retained sample still meets min duration.
   cfg.messages=std::max(uint64_t(100000),uint64_t(std::ceil(cfg.messages*min_seconds*1.12/r.elapsed_s/32))*32);
  }
  std::cerr<<"repeat "<<rep<<" complete rows="<<oi<<"\n";
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 2;}}
