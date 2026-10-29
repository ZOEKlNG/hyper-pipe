#include "storage.hpp"
#include <thread>
#include <iostream>
#include <iomanip>
#include <sched.h>
#include <time.h>
using namespace sf;
uint64_t tick(clockid_t c=CLOCK_MONOTONIC){timespec t;clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
int main(){
 std::cout<<"storage,length,threads,repeat,cycles,wall_ns,total_thread_cpu_ns,cycles_per_s\n"<<std::setprecision(12);
 for(auto mode:{Storage::M1,Storage::M2})for(size_t length:{12,64,512})for(size_t nt:{1,2,4})for(int repeat=0;repeat<5;repeat++){
  Corpus corpus(length,3);std::atomic<size_t>ready{0};std::atomic<bool>go{false};
  struct Stats{uint64_t n=0,wall=0,cpu=0;};std::array<Stats,4>stats{};std::vector<std::thread>threads;
  for(size_t tid=0;tid<nt;tid++)threads.emplace_back([&,tid]{
   cpu_set_t set;CPU_ZERO(&set);CPU_SET(tid,&set);require(!sched_setaffinity(0,sizeof(set),&set),"allocator affinity");
   StorageManager store(mode);std::vector<std::unique_ptr<Session>>sessions;for(size_t i=0;i<128;i++)sessions.push_back(std::make_unique<Session>(i,3,false));
   auto cycle=[&](uint64_t i){auto&s=*sessions[i&127];store.prepare(s,i+tid*4096+1);store.update(s,0,corpus);store.update(s,1,corpus);escape(s.modules.data());store.reset(s);};
   for(size_t i=0;i<4096;i++)cycle(i);ready.fetch_add(1,std::memory_order_release);while(!go.load(std::memory_order_acquire))std::this_thread::yield();
   auto cpu0=tick(CLOCK_THREAD_CPUTIME_ID),t0=tick();uint64_t n=0;
   do{for(size_t k=0;k<4096;k++)cycle(n++);}while(tick()-t0<300000000);
   stats[tid]={n,tick()-t0,tick(CLOCK_THREAD_CPUTIME_ID)-cpu0};
  });
  while(ready.load(std::memory_order_acquire)!=nt)std::this_thread::yield();go.store(true,std::memory_order_release);for(auto&t:threads)t.join();uint64_t n=0,wall=0,cpu=0;for(size_t i=0;i<nt;i++){n+=stats[i].n;wall=std::max(wall,stats[i].wall);cpu+=stats[i].cpu;}
  std::cout<<name(mode)<<','<<length<<','<<nt<<','<<repeat<<','<<n<<','<<wall<<','<<cpu<<','<<double(n)*1e9/wall<<'\n';std::cout.flush();
 }
}
