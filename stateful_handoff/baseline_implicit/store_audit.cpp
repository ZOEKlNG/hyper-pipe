#include "storage.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>
#include <time.h>
#include <barrier>
#include <set>
thread_local bool count_alloc=false;
thread_local uint64_t allocations=0,deallocations=0,bytes=0;
void*operator new(size_t n){if(count_alloc){allocations++;bytes+=n;}if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void*operator new[](size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{if(count_alloc&&p)deallocations++;std::free(p);}
void operator delete[](void*p)noexcept{::operator delete(p);}
void operator delete(void*p,size_t)noexcept{::operator delete(p);}
void operator delete[](void*p,size_t)noexcept{::operator delete(p);}
using namespace sf;
uint64_t now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
int main(){
 std::cout<<"mode,length,cycles,allocations,deallocations,allocated_bytes,pooled_reserved_bytes,retained_string_heap_bytes,logical_payload_bytes,session_payload_capacity,address_span_bytes,payload_cache_lines\n";
 for(auto mode:{Storage::M1,Storage::M2,Storage::M3,Storage::A256,Storage::A512,Storage::A2048})for(size_t len:{12,32,64,128,512}){
  Corpus corpus(len,3);StorageManager store(mode);Session s(0,3,false);auto cycle=[&](uint64_t i){store.prepare(s,i+1);store.update(s,0,corpus);store.update(s,1,corpus);escape(s.modules.data());store.reset(s);};
  for(size_t i=0;i<64;i++)cycle(i);allocations=deallocations=bytes=0;count_alloc=true;for(size_t i=0;i<10000;i++)cycle(i);count_alloc=false;
  size_t heap=0;for(auto&o:s.owned){auto a=reinterpret_cast<uintptr_t>(o.data()),b=reinterpret_cast<uintptr_t>(&o);if(a<b||a>=b+sizeof(o))heap+=o.capacity()+1;}
  store.prepare(s,55);store.update(s,0,corpus);store.update(s,1,corpus);size_t arena_capacity=0;
  for(auto [p,n]:s.blocks)arena_capacity+=n;
  uintptr_t low=UINTPTR_MAX,high=0;std::set<uintptr_t>lines;
  for(size_t m=1;m<3;m++)for(size_t f=0;f<string_count(m);f++){auto v=s.field(m).text[f];auto a=reinterpret_cast<uintptr_t>(v.data);low=std::min(low,a);high=std::max(high,a+v.size);for(uintptr_t line=a/64;line<=(a+v.size-1)/64;line++)lines.insert(line);}
  size_t cap=mode==Storage::M1?6*(len<=15?15:len):mode==Storage::M2?6*s.owned[4].capacity():mode==Storage::M3?6*StorageManager::slab_size(len):arena_capacity;
  std::cout<<name(mode)<<','<<len<<",10000,"<<allocations<<','<<deallocations<<','<<bytes<<','<<store.pool.reserved<<','<<heap<<','<<6*len<<','<<cap<<','<<high-low<<','<<lines.size()<<'\n';
 }
}
