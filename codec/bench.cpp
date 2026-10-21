#include "event.pb.h"
#include "event_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>
#include <zstd.h>
#include <lz4.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <sched.h>
using Clock=std::chrono::steady_clock;
static_assert(std::endian::native==std::endian::little,"fixed layout benchmark currently requires LE host");
struct Header { uint64_t id,seq,timestamp,value0,value1; uint32_t producer,stream,type,flags; uint16_t header_len,version; uint32_t payload_len; };
static_assert(sizeof(Header)==64);
static_assert(offsetof(Header,payload_len)==60);
struct Event {Header h; std::string payload;};
constexpr size_t corpus_n=4096;
std::vector<Event> events;
std::vector<std::string> wires;
volatile uint64_t observable_checksum=0;
inline void escape(const void* p) {asm volatile("" : : "g"(p) : "memory");}
uint64_t sum_header(const Header& h,bool all) {
 if(!all)return h.id+h.seq+h.type;
 return h.id+h.seq+h.timestamp+h.value0+h.value1+h.producer+h.stream+h.type+h.flags;
}
uint64_t sum_payload(const char* p,size_t n) {return n+(n?uint8_t(p[0])+uint8_t(p[n-1]):0);}
void make_events(size_t n,const std::string& pattern) {
 std::mt19937_64 rng(0x61aba1+uint64_t(n)); events.clear();events.reserve(corpus_n);
 for(size_t i=0;i<corpus_n;i++) {
  Event e; e.h={rng(),uint64_t(i+1),1770000000000000000ULL+i*17,rng(),rng(),uint32_t(1+i%32),uint32_t(1+i%64),uint32_t(1+i%7),uint32_t(i%16),64,1,uint32_t(n)};
  e.payload.resize(n);
  for(size_t j=0;j<n;j++) e.payload[j]=pattern=="random"?char(rng()):"INFO device=worker metric=rx_packets status=ok value=1234567890 "[(j+i)%(sizeof("INFO device=worker metric=rx_packets status=ok value=1234567890 ")-1)];
  events.push_back(std::move(e));
 }
}
struct Codec {virtual const char* name()const=0;virtual size_t encode(const Event&,std::string&)=0;virtual uint64_t decode(const std::string&,bool)=0;virtual~Codec()=default;};
struct Fixed:Codec {
 bool checked; explicit Fixed(bool c=false):checked(c){}
 const char* name()const override{return checked?"fixed64_offset_checked":"fixed64_offset_trusted";}
 __attribute__((noinline)) size_t encode(const Event&e,std::string&o) override {
  o.resize(64+e.payload.size());std::memcpy(o.data(),&e.h,64);std::memcpy(o.data()+64,e.payload.data(),e.payload.size());escape(o.data());return o.size();
 }
 __attribute__((noinline)) uint64_t decode(const std::string&w,bool all) override {
  if(checked && w.size()<64)std::abort();Header h;std::memcpy(&h,w.data(),64);if(checked && (h.version!=1 || h.header_len<64 || h.header_len>w.size() || h.payload_len>w.size()-h.header_len))std::abort();return sum_header(h,all)+(all?sum_payload(w.data()+h.header_len,h.payload_len):0);
 }
};
template<class PB,bool Fresh=false> struct Protobuf:Codec {
 PB msg;
 const char* name()const override {if constexpr(std::is_same_v<PB,bench::FixedEvent>)return Fresh?"protobuf_fixed_fresh":"protobuf_fixed_reuse";else return Fresh?"protobuf_varint_fresh":"protobuf_varint_reuse";}
 void set(PB&m,const Event&e){m.set_id(e.h.id);m.set_seq(e.h.seq);m.set_timestamp(e.h.timestamp);m.set_value0(e.h.value0);m.set_value1(e.h.value1);m.set_producer(e.h.producer);m.set_stream(e.h.stream);m.set_type(e.h.type);m.set_flags(e.h.flags);m.set_payload(e.payload);}
 __attribute__((noinline)) size_t encode(const Event&e,std::string&o) override {
  if constexpr(Fresh){PB m;set(m,e);m.SerializeToString(&o);}else{set(msg,e);msg.SerializeToString(&o);}escape(o.data());return o.size();
 }
 uint64_t get(const PB&m,bool all){if(!all)return m.id()+m.seq()+m.type();return m.id()+m.seq()+m.timestamp()+m.value0()+m.value1()+m.producer()+m.stream()+m.type()+m.flags()+sum_payload(m.payload().data(),m.payload().size());}
 __attribute__((noinline)) uint64_t decode(const std::string&w,bool all) override {
  if constexpr(Fresh){PB m;if(!m.ParseFromArray(w.data(),int(w.size())))std::abort();return get(m,all);}else{if(!msg.ParseFromArray(w.data(),int(w.size())))std::abort();return get(msg,all);}
 }
};
struct Flat:Codec {
 flatbuffers::FlatBufferBuilder builder{2048};bool checked;
 explicit Flat(bool c=false):checked(c){}
 const char* name()const override{return checked?"flatbuffers_verified":"flatbuffers_trusted";}
 __attribute__((noinline)) size_t build_no_copy(const Event&e) {
  builder.Clear();auto p=builder.CreateVector(reinterpret_cast<const uint8_t*>(e.payload.data()),e.payload.size());
  auto root=fbbench::CreateEvent(builder,e.h.id,e.h.seq,e.h.timestamp,e.h.value0,e.h.value1,e.h.producer,e.h.stream,e.h.type,e.h.flags,p);builder.Finish(root);escape(builder.GetBufferPointer());return builder.GetSize();
 }

 __attribute__((noinline)) size_t encode(const Event&e,std::string&o) override {
  builder.Clear();auto p=builder.CreateVector(reinterpret_cast<const uint8_t*>(e.payload.data()),e.payload.size());
  auto root=fbbench::CreateEvent(builder,e.h.id,e.h.seq,e.h.timestamp,e.h.value0,e.h.value1,e.h.producer,e.h.stream,e.h.type,e.h.flags,p);builder.Finish(root);
  o.assign(reinterpret_cast<const char*>(builder.GetBufferPointer()),builder.GetSize());escape(o.data());return o.size();
 }
 __attribute__((noinline)) uint64_t decode(const std::string&w,bool all) override {
  if(checked){flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(w.data()),w.size());if(!fbbench::VerifyEventBuffer(v))std::abort();}
  auto m=fbbench::GetEvent(w.data());if(!all)return m->id()+m->seq()+m->type();auto p=m->payload();return m->id()+m->seq()+m->timestamp()+m->value0()+m->value1()+m->producer()+m->stream()+m->type()+m->flags()+sum_payload(reinterpret_cast<const char*>(p->data()),p->size());
 }
};
struct Json:Codec {
 const char* name()const override{return "json_nlohmann_hex";}
 static std::string hex(const std::string&s){constexpr char h[]="0123456789abcdef";std::string o(s.size()*2,'0');for(size_t i=0;i<s.size();i++){o[2*i]=h[uint8_t(s[i])>>4];o[2*i+1]=h[uint8_t(s[i])&15];}return o;}
 __attribute__((noinline)) size_t encode(const Event&e,std::string&o) override {
  nlohmann::json j={{"id",e.h.id},{"seq",e.h.seq},{"timestamp",e.h.timestamp},{"value0",e.h.value0},{"value1",e.h.value1},{"producer",e.h.producer},{"stream",e.h.stream},{"type",e.h.type},{"flags",e.h.flags},{"payload_hex",hex(e.payload)}};o=j.dump();escape(o.data());return o.size();
 }
 __attribute__((noinline)) uint64_t decode(const std::string&w,bool all) override {
  auto j=nlohmann::json::parse(w);uint64_t s=j["id"].get<uint64_t>()+j["seq"].get<uint64_t>()+j["type"].get<uint64_t>();if(!all)return s;
  s+=j["timestamp"].get<uint64_t>()+j["value0"].get<uint64_t>()+j["value1"].get<uint64_t>()+j["producer"].get<uint64_t>()+j["stream"].get<uint64_t>()+j["flags"].get<uint64_t>();
  const auto&p=j["payload_hex"].get_ref<const std::string&>();auto nib=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};size_t n=p.size()/2;
  return s+n+(n?((nib(p[0])<<4)|nib(p[1]))+((nib(p[p.size()-2])<<4)|nib(p.back())):0);
 }
};
struct Measurement {std::string operation;size_t iterations;double ns;uint64_t checksum;};
template<class Fn> Measurement measure(const std::string&op,Fn&&fn) {
 uint64_t check=0;for(size_t i=0;i<8192;i++)check+=fn(i%corpus_n);observable_checksum=check;
 size_t count=4096;double elapsed=0;
 do {auto a=Clock::now();for(size_t i=0;i<count;i++)check+=fn(i%corpus_n);elapsed=std::chrono::duration<double,std::nano>(Clock::now()-a).count();if(elapsed>=20000000. || count>=(1u<<24))break;count*=2;}while(true);
 observable_checksum=check;return {op,count,elapsed/double(count),check};
}
int main(int argc,char**argv){
 cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(0,&cpus);int pin=sched_setaffinity(0,sizeof(cpus),&cpus);std::cerr<<"pin_cpu0="<<pin<<" protobuf="<<GOOGLE_PROTOBUF_VERSION<<" flatbuffers="<<FLATBUFFERS_VERSION_MAJOR<<"."<<FLATBUFFERS_VERSION_MINOR<<"."<<FLATBUFFERS_VERSION_REVISION<<"\n";
 bool export_only=argc>1 && std::string(argv[1])=="--export-only";
 std::cout<<"codec,payload_bytes,pattern,operation,repeat,wire_bytes_mean,iterations,ns_per_op,checksum\n";
 std::vector<std::unique_ptr<Codec>> codecs;codecs.emplace_back(new Fixed);codecs.emplace_back(new Fixed(true));codecs.emplace_back(new Protobuf<bench::Event>);codecs.emplace_back(new Protobuf<bench::FixedEvent>);codecs.emplace_back(new Protobuf<bench::Event,true>);codecs.emplace_back(new Flat);codecs.emplace_back(new Flat(true));codecs.emplace_back(new Json);
 for(size_t n:{0,64,256,1024})for(std::string pattern:{"random","repetitive"}) {
  make_events(n,pattern);
  for(auto&c:codecs) {
   wires.clear();wires.resize(corpus_n);size_t total=0;
   for(size_t i=0;i<corpus_n;i++){total+=c->encode(events[i],wires[i]);uint64_t got=c->decode(wires[i],true);uint64_t want=sum_header(events[i].h,true)+sum_payload(events[i].payload.data(),n);if(got!=want){std::cerr<<"validation failed "<<c->name()<<" at "<<i<<" got="<<got<<" want="<<want<<"\n";return 2;}}
   if(std::string(c->name())=="fixed64_offset_trusted" || std::string(c->name())=="protobuf_varint_reuse") {
    std::string path="corpus/"+std::string(c->name())+"_"+std::to_string(n)+"_"+pattern+".bin";
    std::ofstream file(path,std::ios::binary);if(!file){std::cerr<<"cannot write "<<path<<"\n";return 4;}
    for(const auto&w:wires){uint32_t len=w.size();file.write(reinterpret_cast<const char*>(&len),4);file.write(w.data(),w.size());}
   }
   if(export_only)continue;
   for(int rep=0;rep<5;rep++)for(std::string op:{"encode","decode_all","decode_few","roundtrip"}){
    if(std::string(c->name())=="flatbuffers_verified"&&op=="encode")continue;
    std::string out;out.reserve(4096);
    Measurement m;
    if(op=="encode")m=measure(op,[&](size_t i){return c->encode(events[i],out);});
    else if(op=="decode_all")m=measure(op,[&](size_t i){return c->decode(wires[i],true);});
    else if(op=="decode_few")m=measure(op,[&](size_t i){return c->decode(wires[i],false);});
    else m=measure(op,[&](size_t i){c->encode(events[i],out);return c->decode(out,true);});
    std::cout<<c->name()<<","<<n<<","<<pattern<<","<<op<<","<<rep<<","<<double(total)/corpus_n<<","<<m.iterations<<","<<std::setprecision(9)<<m.ns<<","<<m.checksum<<"\n";
   }
   std::cerr<<"done "<<c->name()<<" n="<<n<<" "<<pattern<<"\n";
  }
 }
 return 0;
}
