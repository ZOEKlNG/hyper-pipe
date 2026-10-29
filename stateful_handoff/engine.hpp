#pragma once
#include "encoding.hpp"
#include <chrono>
#include <functional>
#include <immintrin.h>
#include <sched.h>
#include <thread>
#include <time.h>
#include <sys/resource.h>
namespace sf {
inline uint64_t ns(clockid_t c=CLOCK_MONOTONIC){timespec t;clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
inline void pin(int cpu){cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu,&mask);require(!sched_setaffinity(0,sizeof(mask),&mask),"CPU pin failed");}
struct ReturnRing {
 Counter head,tail;std::vector<Session*>data;
 explicit ReturnRing(size_t n):data(std::bit_ceil(n)){}
 void push(Session*s){auto h=head.value.load(std::memory_order_relaxed);require(h-tail.value.load(std::memory_order_acquire)<data.size(),"return ring overflow");data[h&(data.size()-1)]=s;head.value.store(h+1,std::memory_order_release);}
 Session*pop(){auto t=tail.value.load(std::memory_order_relaxed);if(t==head.value.load(std::memory_order_acquire))return nullptr;auto*p=data[t&(data.size()-1)];tail.value.store(t+1,std::memory_order_release);return p;}
};
struct Engine {
 Config cfg;Corpus corpus;StorageManager store;RingMemory ring;
 std::vector<std::unique_ptr<Session>>pool;std::vector<Session*>free;
 std::array<Session*,ACTIVE>active{};std::array<std::shared_ptr<Session>,ACTIVE>leases;
 ReturnRing returns;Counter head,tail,ready,go;std::atomic<bool>done{false};
 size_t free_low=0,retired=0,retired_peak=0;uint64_t next_id=1,reclaimed=0,immediate=0,hooked=0;
 std::thread::id worker_id;
 explicit Engine(Config c):cfg(c),corpus(c.length,c.modules),store(c.storage),ring(c.handoff,c.length,c.modules,c.queue),returns(ACTIVE+c.queue+1){
  require(c.queue&&std::has_single_bit(c.queue),"queue must be power of two");
  require((c.life!=Life::L1&&c.life!=Life::L2)||c.storage==Storage::M2,"L1/L2 matrix uses string-reuse owner-local storage");
  size_t count=ACTIVE+c.queue+1;pool.reserve(count);free.reserve(count);
  for(size_t i=0;i<count;i++)pool.push_back(std::make_unique<Session>(i,c.modules,c.handoff==Handoff::H6,c.handoff==Handoff::H4));
  // Warm all capacity while simultaneously live, then return it. M1 deliberately
  // destroys strings; M2 retains capacity; M3/M4 retain pool blocks.
  for(auto&p:pool){store.prepare(*p,++next_id);store.update(*p,0,corpus);store.update(*p,1,corpus);}
  for(auto&p:pool){store.reset(*p);free.push_back(p.get());}
  free_low=free.size();worker_id=std::this_thread::get_id();
 }
 void recycle(Session*s,bool already_reset=false){if(!already_reset)store.reset(*s);free.push_back(s);reclaimed++;}
 void drain_returns(){while(auto*s=returns.pop())recycle(s,true);}
 Session*acquire(size_t index){
  drain_returns();require(!free.empty(),"session pool exhausted; retire budget insufficient");auto*s=free.back();free.pop_back();free_low=std::min(free_low,free.size());store.prepare(*s,next_id++);active[index]=s;
  if(cfg.life==Life::L1){leases[index]=std::shared_ptr<Session>(s,[this](Session*p){if(std::this_thread::get_id()==worker_id)recycle(p);else{store.reset(*p);returns.push(p);}});}
  return s;
 }
 void reclaim_hook(Header&h){if(h.retiring){recycle(h.retiring);h.retiring=nullptr;retired--;}}
 void close(Session*s,size_t index){
  active[index]=nullptr;
  if(cfg.handoff==Handoff::H1||cfg.life==Life::L0){recycle(s);return;}
  if(cfg.life==Life::L1){leases[index].reset();return;}
  if(cfg.life==Life::L2){
   // The final accepted record may precede a dropped close record. L2's return
   // protocol for drop stress therefore uses L3, not a guessed close marker.
   return;
  }
  uint64_t read=tail.value.load(std::memory_order_acquire);
  if(s->last_seq==UINT64_MAX||read>s->last_seq){recycle(s);immediate++;}
  else{
   auto&slot=ring.header(s->last_seq);require(!slot.retiring,"multiple retirements on one last-log slot");
   // Logger never reads/writes retiring. This Worker-only field may be attached
   // after publish; all payload bytes remain immutable until tail advances.
   slot.retiring=s;retired++;retired_peak=std::max(retired_peak,retired);hooked++;
  }
 }
 void drain_hooks(){for(size_t i=0;i<cfg.queue;i++)reclaim_hook(ring.header(i));drain_returns();}
 size_t metadata_bytes()const{
  size_t n=pool.size()*sizeof(Session);for(auto&p:pool)n+=p->modules.capacity()*sizeof(ModuleState)+p->frozen.capacity()*sizeof(Export)+p->owned.capacity()*sizeof(std::string)+p->slabs.capacity()*sizeof(char*)+p->blocks.capacity()*sizeof(std::pair<char*,size_t>)+(p->bulk?sizeof(*p->bulk):0);return n;
 }
 size_t string_heap_capacity()const{size_t n=0;if(cfg.storage==Storage::M1||cfg.storage==Storage::M2)for(auto&p:pool)for(auto&s:p->owned){auto addr=reinterpret_cast<uintptr_t>(s.data()),lo=reinterpret_cast<uintptr_t>(&s);if(addr<lo||addr>=lo+sizeof(s))n+=s.capacity()+1;}return n;}
};
struct RunStats {
 uint64_t offered=0,accepted=0,dropped=0,received=0,errors=0,wire_bytes=0,checksum=0,worker_cpu=0,logger_cpu=0,worker_spins=0,logger_spins=0;
 uint64_t reclaimed=0,hooked=0,immediate=0,retired_peak=0,free_low=0,pool_reserved=0,pool_growth=0,metadata=0,string_capacity=0,ring_bytes=0,return_bytes=0;double wall=0;long maxrss=0;
};
template<bool Verify> RunStats pipeline(Engine&e,uint64_t offers,bool drop_new=false,bool pause_logger=false){
 if constexpr(Verify)e.store.enable_poison();
 RunStats st;st.offered=offers;auto&c=e.cfg;require(!(drop_new&&c.life==Life::L2),"drop-new L2 needs an explicit close control marker; use L3 stress");
 uint64_t received=0,errors=0,wirebytes=0,checksum=0,loggercpu=0,loggerpolls=0;
 auto logger=[&]{
  pin(c.logger_cpu);Encoder enc(c.length);auto tmp=temp_bytes(1,1);auto&s=*e.pool.back();e.ready.value.store(1,std::memory_order_release);while(!e.go.value.load(std::memory_order_acquire))_mm_pause();
  auto cpu0=ns(CLOCK_THREAD_CPUTIME_ID);if(pause_logger)std::this_thread::sleep_for(std::chrono::milliseconds(2));uint64_t t=0;
  for(;;){uint64_t h=e.head.value.load(std::memory_order_acquire);if(t==h){if(e.done.load(std::memory_order_acquire)&&t==e.head.value.load(std::memory_order_acquire))break;loggerpolls++;_mm_pause();continue;}
   auto&slot=e.ring.header(t);if(slot.sequence!=t)errors++;auto n=enc.encode(slot,e.ring.body(slot),c.handoff);if constexpr(Verify){if(!enc.verify(slot,n,e.corpus))errors++;}
   wirebytes+=n;checksum+=n+enc.wire[0]+enc.wire[n-1];
   if(c.life==Life::L1)slot.hold.reset();
   else if(c.life==Life::L2){if(slot.phase==3){e.store.reset(*slot.session);e.returns.push(slot.session);}slot.temp_used=slot.used=slot.count=0;slot.mask=0;}
   if(c.logger_zero){slot.temp_used=slot.used=slot.count=0;slot.mask=0;}
   received++;t++;e.tail.value.store(t,std::memory_order_release);
  }loggercpu=ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;
 };
 pin(0);std::thread consumer(logger);while(!e.ready.value.load(std::memory_order_acquire))_mm_pause();auto cpu0=ns(CLOCK_THREAD_CPUTIME_ID),wall0=ns();e.go.value.store(1,std::memory_order_release);uint64_t head=0;
 for(uint64_t i=0;i<offers;i++){
  size_t index=i&(ACTIVE-1);Session*s=e.active[index];if(!s)s=e.acquire(index);unsigned phase=s->phase;e.store.update(*s,phase,e.corpus);
  auto tmp=temp_bytes(s->id,phase);bool available=head-e.tail.value.load(std::memory_order_acquire)<c.queue;
  if(!available&&!drop_new){do{st.worker_spins++;_mm_pause();e.drain_returns();available=head-e.tail.value.load(std::memory_order_acquire)<c.queue;}while(!available);}
  if(available){auto&slot=e.ring.header(head);e.reclaim_hook(slot);slot.sequence=head;build(slot,e.ring.body(slot),*s,phase,c,tmp);
   if(c.life==Life::L1)slot.hold=e.leases[index];s->last_seq=head;head++;st.accepted++;e.head.value.store(head,std::memory_order_release);
  }else st.dropped++;
  if constexpr(Verify)poison(tmp.data(),tmp.size()); // Stack-only xff must have been copied.
  if(phase==3)e.close(s,index);else s->phase=phase+1;
 }
 e.done.store(true,std::memory_order_release);while(e.tail.value.load(std::memory_order_acquire)<head){st.worker_spins++;e.drain_returns();_mm_pause();}consumer.join();
 e.drain_hooks();
 // Partial active sessions have no outstanding records after final drain.
 for(size_t i=0;i<ACTIVE;i++)if(e.active[i]){auto*s=e.active[i];e.active[i]=nullptr;if(c.life==Life::L1)e.leases[i].reset();else e.recycle(s);}
 e.drain_returns();st.wall=double(ns()-wall0)/1e9;st.worker_cpu=ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;st.logger_cpu=loggercpu;st.logger_spins=loggerpolls;
 st.received=received;st.errors=errors;st.wire_bytes=wirebytes;st.checksum=checksum;st.reclaimed=e.reclaimed;st.hooked=e.hooked;st.immediate=e.immediate;st.retired_peak=e.retired_peak;st.free_low=e.free_low;
 st.pool_reserved=e.store.pool.reserved;st.pool_growth=e.store.pool.allocations;st.metadata=e.metadata_bytes();st.string_capacity=e.string_heap_capacity();st.ring_bytes=e.ring.layout.stride*c.queue;st.return_bytes=e.returns.data.size()*sizeof(Session*);
 rusage usage;getrusage(RUSAGE_SELF,&usage);st.maxrss=usage.ru_maxrss;
 require(!errors&&st.offered==st.accepted+st.dropped&&st.received==st.accepted,"pipeline validation failed");require(e.free.size()==e.pool.size()&&e.retired==0,"shutdown did not reclaim all sessions");
 if constexpr(Verify){std::vector<bool>seen(e.pool.size());for(auto*p:e.free){require(p->index<seen.size()&&!seen[p->index]&&e.pool[p->index].get()==p,"duplicate/invalid free session");seen[p->index]=true;}}return st;
}
} // namespace sf
