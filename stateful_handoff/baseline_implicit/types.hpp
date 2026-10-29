#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace sf {
constexpr size_t MAX_MODULES=8,STRINGS=4,ACTIVE=4096,QUEUE=4096;
constexpr uint64_t TEMP_BIT=uint64_t(1)<<63;
struct View {const char* data=nullptr;size_t size=0;std::string_view str()const{return {data,size};}};
struct Export {
 uint64_t extracted=0;
 std::array<uint64_t,2>value{};
 std::array<View,STRINGS>text{};
};
static_assert(std::is_trivially_copyable_v<Export>);
struct ModuleState:Export {std::array<uint64_t,8>private_engine_state{};};
inline size_t string_count(size_t mod){return mod==0?0:mod==2?4:2;}
inline uint64_t local_available(size_t mod){return 3|(((uint64_t(1)<<string_count(mod))-1)<<2);}
inline uint64_t scalar_value(uint64_t id,unsigned phase,size_t mod,size_t field){return field? id*131+phase*1009+mod*17 : id*17+mod+1;}
inline void escape(const void*p){asm volatile(""::"g"(p):"memory");}
inline void require(bool b,const char*msg){if(!b)throw std::runtime_error(msg);}
enum class Storage{M1,M2,M3,A256,A512,A2048};
inline const char*name(Storage m){switch(m){case Storage::M1:return"M1_string_new";case Storage::M2:return"M2_string_reuse";case Storage::M3:return"M3_slab";case Storage::A256:return"M4_arena256";case Storage::A512:return"M4_arena512";case Storage::A2048:return"M4_arena2048";}return"?";}
enum class Handoff{H1,H2,H3,H4,H5,H6};
inline const char*name(Handoff h){return std::array<const char*,6>{"H1_deep","H2_tape","H3_selective","H4_bulk","H5_modules_copy","H6_versioned_ptr"}[size_t(h)];}
enum class Life{L0,L1,L2,L3};
inline const char*name(Life l){return std::array<const char*,4>{"L0_owned","L1_shared_lease","L2_return_reset","L3_piggyback"}[size_t(l)];}
struct Config {Storage storage=Storage::M2;Handoff handoff=Handoff::H5;Life life=Life::L3;size_t length=64,modules=3;bool sparse=false;size_t queue=QUEUE;int logger_cpu=1;bool logger_zero=false;};
inline uint64_t rule_mask(size_t modules,unsigned phase,bool sparse){
 uint64_t available=0;for(size_t m=0;m<modules;m++)if(phase||m<2)available|=local_available(m)<<(m*6);
 if(phase==1||phase==2)available|=TEMP_BIT;
 if(sparse||phase==2){
  uint64_t want=1; // common.value0
  if(modules>1)want|=uint64_t(1)<<(6+2);
  if(modules>2)want|=uint64_t(1)<<(12+2);
  if(available&TEMP_BIT)want|=TEMP_BIT;else want|=2;
  if(std::popcount(want)<4&&modules>1)want|=uint64_t(1)<<6;
  return want&available;
 }
 if(phase==3){uint64_t want=3;if(modules>1)want|=uint64_t(1)<<(6+2);if(modules>2)want|=uint64_t(1)<<(12+2);return want&available;}
 return available;
}
struct Corpus {
 size_t length,modules;std::vector<std::string>data;
 Corpus(size_t len,size_t mods):length(len),modules(mods),data(ACTIVE*mods*STRINGS){
  for(size_t i=0;i<ACTIVE;i++)for(size_t m=1;m<mods;m++)for(size_t f=0;f<string_count(m);f++){
   auto&s=data[(i*mods+m)*STRINGS+f];s.resize(len);uint64_t v=(i+1)*0x9e3779b97f4a7c15ULL+m*29+f;
   for(auto&c:s){v^=v<<13;v^=v>>7;v^=v<<17;c=char(v);}
  }
 }
 std::string_view get(uint64_t id,size_t mod,size_t field)const{return data[((id&(ACTIVE-1))*modules+mod)*STRINGS+field];}
};
inline std::array<char,32>temp_bytes(uint64_t id,unsigned phase){std::array<char,32>a;for(size_t i=0;i<a.size();i++)a[i]=char((id*19+i*7+phase*31)&255);return a;}
inline void poison(char*p,size_t n){for(size_t i=0;i<n;i++)static_cast<volatile char*>(p)[i]=char(0xdd);}
struct Session {
 size_t index=0;uint64_t id=0,last_seq=UINT64_MAX,extracted_bits=0;unsigned phase=0;
 std::vector<ModuleState>modules;
 std::vector<Export>frozen;
 std::unique_ptr<std::array<Export,MAX_MODULES>>bulk;
 std::vector<std::string>owned;
 std::vector<char*>slabs;
 std::vector<std::pair<char*,size_t>>blocks;
 size_t arena_used=0;bool reset_done=false;
 Session(size_t i,size_t mods,bool freeze,bool monolithic=false):index(i),modules(mods),frozen(freeze?mods*4:0),bulk(monolithic?std::make_unique<std::array<Export,MAX_MODULES>>():nullptr),owned(mods*STRINGS),slabs(mods*STRINGS,nullptr){blocks.reserve(mods*STRINGS);}
 Export&field(size_t m){return bulk?(*bulk)[m]:static_cast<Export&>(modules[m]);}
 const Export&field(size_t m)const{return bulk?(*bulk)[m]:static_cast<const Export&>(modules[m]);}
};
struct alignas(128) Counter{std::atomic<uint64_t>value{0};};
} // namespace sf
