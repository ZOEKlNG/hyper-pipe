#pragma once
#include "types.hpp"
#include <algorithm>
#include <cstdlib>
namespace sf {
// Worker-owned pools, shared across that Worker's sessions, not a concurrent
// cross-Worker allocator. Oversize arena fields get a contiguous larger block.
struct BlockPool {
 std::vector<std::vector<char*>>free;
 using Page=std::unique_ptr<char[],void(*)(char*)>;
 std::vector<std::pair<Page,size_t>>owned;
 size_t reserved=0,allocations=0,live=0,peak_live=0;bool debug_poison=false;
 BlockPool():free(128){}
 char*get(size_t bytes){
  require(bytes%32==0&&bytes/32<free.size(),"block class outside range");auto&f=free[bytes/32];char*p;
  if(f.empty()){
   constexpr size_t page_bytes=65536;Page b(static_cast<char*>(std::aligned_alloc(64,page_bytes)),[](char*p){std::free(p);});if(!b)throw std::bad_alloc();p=b.get();
   for(size_t offset=bytes;offset+bytes<=page_bytes;offset+=bytes)f.push_back(p+offset);
   owned.emplace_back(std::move(b),page_bytes);reserved+=page_bytes;allocations++;
  }
  else{p=f.back();f.pop_back();}live+=bytes;peak_live=std::max(live,peak_live);return p;
 }
 void put(char*p,size_t bytes){if(debug_poison)poison(p,bytes);free[bytes/32].push_back(p);live-=bytes;}
};
struct StorageManager {
 Storage mode;size_t block_size=0;BlockPool pool;bool debug_poison=false;
 void enable_poison(){debug_poison=true;pool.debug_poison=true;}
 explicit StorageManager(Storage m):mode(m){block_size=m==Storage::A256?256:m==Storage::A512?512:m==Storage::A2048?2048:0;}
 static size_t slab_size(size_t n){size_t s=32;while(s<n)s*=2;return s;}
 void put_string(Session&s,size_t mod,size_t field,std::string_view v){
  size_t i=mod*STRINGS+field;char*p=nullptr;
  if(mode==Storage::M1||mode==Storage::M2){auto&o=s.owned[i];o.assign(v);p=o.data();}
  else if(mode==Storage::M3){p=pool.get(slab_size(v.size()));s.slabs[i]=p;std::memcpy(p,v.data(),v.size());}
  else{
   if(s.blocks.empty()||s.arena_used+v.size()>s.blocks.back().second){
    size_t cap=std::max(block_size,(v.size()+block_size-1)/block_size*block_size);
    p=pool.get(cap);s.blocks.emplace_back(p,cap);s.arena_used=0;
   }
   p=s.blocks.back().first+s.arena_used;std::memcpy(p,v.data(),v.size());s.arena_used+=v.size();
  }
  s.field(mod).text[field]={p,v.size()};
 }
 void prepare(Session&s,uint64_t id){
  s.id=id;s.phase=0;s.extracted_bits=0;s.last_seq=UINT64_MAX;s.reset_done=false;
  for(size_t m=0;m<s.modules.size();m++){s.field(m)=Export{};s.modules[m].private_engine_state[0]=id;}
 }
 void update(Session&s,unsigned phase,const Corpus&corpus){
  s.phase=phase;s.extracted_bits=(phase==1||phase==2)?TEMP_BIT:0;
  // TLS fields are extracted once at handshake; remaining protocol strings
  // once at request stage. Subsequent logs borrow immutable bytes, not mbufs.
  if(phase==0&&s.modules.size()>1)for(size_t f=0;f<string_count(1);f++)put_string(s,1,f,corpus.get(s.id,1,f));
  if(phase==1)for(size_t m=2;m<s.modules.size();m++)for(size_t f=0;f<string_count(m);f++)put_string(s,m,f,corpus.get(s.id,m,f));
  for(size_t m=0;m<s.modules.size();m++){
   auto&v=s.field(m);if(phase||m<2)v.extracted=local_available(m);else v.extracted=0;
   s.extracted_bits|=v.extracted<<(m*6);
   v.value={scalar_value(s.id,phase,m,0),scalar_value(s.id,phase,m,1)};s.modules[m].private_engine_state[1]+=phase+1;
  }
 }
 void reset(Session&s){
  for(size_t m=1;m<s.modules.size();m++)for(size_t f=0;f<string_count(m);f++){
   size_t i=m*STRINGS+f;auto v=s.field(m).text[f];
   if(mode==Storage::M1||mode==Storage::M2){auto&o=s.owned[i];if(debug_poison&&!o.empty())poison(o.data(),o.size());
    if(mode==Storage::M1){std::destroy_at(&o);std::construct_at(&o);}else o.clear();
   }else if(mode==Storage::M3){if(s.slabs[i]){pool.put(s.slabs[i],slab_size(v.size));s.slabs[i]=nullptr;}}
  }
  if(block_size){for(auto [p,n]:s.blocks)pool.put(p,n);s.blocks.clear();s.arena_used=0;}
  for(size_t m=0;m<s.modules.size();m++)s.field(m)=Export{};s.reset_done=true;
 }
};
} // namespace sf
