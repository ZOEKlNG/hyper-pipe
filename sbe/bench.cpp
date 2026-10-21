#include "generated/sbebench/Event.h"
#include "generated/sbebench/MessageHeader.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sched.h>

using Clock = std::chrono::steady_clock;
static_assert(std::endian::native == std::endian::little);
struct Header { uint64_t id,seq,timestamp,value0,value1; uint32_t producer,stream,type,flags; uint16_t header_len,version; uint32_t payload_len; };
static_assert(sizeof(Header)==64);
struct Event { Header h; std::string payload; };
constexpr size_t corpus_n=4096;
volatile uint64_t observable_checksum=0;
inline void escape(const void* p) { asm volatile("" : : "g"(p) : "memory"); }
uint64_t want(const Event&e,bool all) {
 auto&h=e.h; uint64_t n=e.payload.size();
 uint64_t s=h.id+h.seq+h.type;
 if(all) s+=h.timestamp+h.value0+h.value1+h.producer+h.stream+h.flags+n+(n?uint8_t(e.payload[0])+uint8_t(e.payload[n-1]):0);
 return s;
}
std::vector<Event> load_events(size_t payload_bytes) {
 const char* configured=std::getenv("BENCH_CORPUS_DIR");
 std::string dir=configured?configured:"../codec/corpus";
 std::string path=dir+"/fixed64_offset_trusted_"+std::to_string(payload_bytes)+"_random.bin";
 std::ifstream f(path,std::ios::binary); if(!f)throw std::runtime_error("cannot open "+path);
 std::vector<Event> events;events.reserve(corpus_n);
 for(size_t i=0;i<corpus_n;i++) {
  uint32_t n;f.read(reinterpret_cast<char*>(&n),4);if(!f || n!=64+payload_bytes)throw std::runtime_error("bad corpus length");
  std::string w(n,'\0');f.read(w.data(),n);if(!f)throw std::runtime_error("truncated corpus");
  Event e;std::memcpy(&e.h,w.data(),64);
  if(e.h.header_len!=64 || e.h.version!=1 || e.h.payload_len!=payload_bytes)throw std::runtime_error("bad fixed header");
  e.payload.assign(w.data()+64,payload_bytes);events.push_back(std::move(e));
 }
 if(f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("extra corpus records");
 return events;
}
struct Codec { virtual const char* name()const=0;virtual size_t encode(const Event&,std::string&)=0;virtual uint64_t decode(const std::string&,bool)=0;virtual~Codec()=default; };
struct Sbe : Codec {
 const char* name() const override {
#ifdef SBE_NO_BOUNDS_CHECK
  return "sbe_generated_trusted";
#else
  return "sbe_generated_validated";
#endif
 }
 __attribute__((noinline)) size_t encode(const Event&e,std::string&out) override {
  out.resize(68+e.payload.size());sbebench::Event m;
  m.wrapAndApplyHeader(out.data(),0,out.size())
    .id(e.h.id).seq(e.h.seq).timestamp(e.h.timestamp).value0(e.h.value0).value1(e.h.value1)
    .producer(e.h.producer).stream(e.h.stream).type(e.h.type).flags(e.h.flags)
    .putPayload(e.payload.data(),uint32_t(e.payload.size()));
  escape(out.data());return sbebench::MessageHeader::encodedLength()+m.encodedLength();
 }
 __attribute__((noinline)) uint64_t decode(const std::string&wire,bool all) override {
#ifndef SBE_NO_BOUNDS_CHECK
  if(wire.size()<68)throw std::runtime_error("frame too short");
#endif
  // Generated API takes char*, but all decode access below is read-only.
  char* p=const_cast<char*>(wire.data());sbebench::MessageHeader h(p,wire.size());
#ifndef SBE_NO_BOUNDS_CHECK
  if(h.schemaId()!=61 || h.templateId()!=1 || h.version()!=1 || h.blockLength()!=56)
    throw std::runtime_error("unsupported benchmark schema");
#endif
  sbebench::Event m;m.wrapForDecode(p,8,h.blockLength(),h.version(),wire.size());
#ifndef SBE_NO_BOUNDS_CHECK
  // Generated payloadLength() reads a u32 before advancing the cursor. The
  // minimum length and exact block length guards above make that read safe.
  const auto length=m.payloadLength();
  if(length!=wire.size()-68)throw std::runtime_error("payload length mismatch");
#endif
  uint64_t s=m.id()+m.seq()+m.type();
  if(!all)return s;
  s+=m.timestamp()+m.value0()+m.value1()+m.producer()+m.stream()+m.flags();
  const uint32_t n=m.payloadLength();const char* payload=m.payload();
  return s+n+(n?uint8_t(payload[0])+uint8_t(payload[n-1]):0);
 }
};
struct Measurement {size_t iterations;double ns;uint64_t checksum;};
template<class Fn> Measurement measure(Fn&& fn) {
 uint64_t check=0;for(size_t i=0;i<8192;i++)check+=fn(i%corpus_n);observable_checksum=check;
 size_t count=4096;double elapsed=0;
 for(;;) {
  auto a=Clock::now();for(size_t i=0;i<count;i++)check+=fn(i%corpus_n);
  elapsed=std::chrono::duration<double,std::nano>(Clock::now()-a).count();
  // Break before changing count, so elapsed is always paired with the number
  // of operations actually timed, including when the calibration cap is hit.
  if(elapsed>=20000000. || count>=(1u<<24))break;
  count*=2;
 }
 observable_checksum=check;return {count,elapsed/double(count),check};
}
int main(int argc,char**argv) {
 try {
 cpu_set_t set;CPU_ZERO(&set);CPU_SET(0,&set);int pin=sched_setaffinity(0,sizeof(set),&set);
 std::cerr<<"pin_cpu0="<<pin<<" SBE=1.35.0 generator=official_java_jar header=8 root=56 payload_length=4\n";
 if(pin!=0)throw std::runtime_error("cannot bind CPU0");
 bool selfcheck=argc>1 && std::string(argv[1])=="--self-check";
 Sbe concrete;Codec* codec=&concrete;
 std::cout<<"codec,payload_bytes,pattern,operation,repeat,wire_bytes_mean,iterations,ns_per_op,checksum\n";
 for(size_t bytes:{0,64,256,1024}) {
  auto events=load_events(bytes);std::vector<std::string>wires(corpus_n);size_t total=0;
  for(size_t i=0;i<corpus_n;i++) {
   total+=codec->encode(events[i],wires[i]);
   if(codec->decode(wires[i],true)!=want(events[i],true) || codec->decode(wires[i],false)!=want(events[i],false))throw std::runtime_error("checksum mismatch");
  }
#ifndef SBE_NO_BOUNDS_CHECK
  // Bounded decoder smoke checks are deliberately outside measured batches.
  for(int bad=0;bad<3;bad++) {
   std::string w=wires[0];if(bad==0)w.resize(7);else if(bad==1)w[4]^=1;else w[64]^=1;
   bool rejected=false;try {codec->decode(w,true);}catch(const std::runtime_error&){rejected=true;}
   if(!rejected)throw std::runtime_error("malformed frame accepted");
  }
#endif
  if(selfcheck)continue;
  for(int repeat=0;repeat<5;repeat++)for(std::string op:{"encode","decode_all","decode_few","roundtrip"}) {
   std::string out;out.reserve(4096);Measurement m;
   if(op=="encode")m=measure([&](size_t i){return codec->encode(events[i],out);});
   else if(op=="decode_all")m=measure([&](size_t i){return codec->decode(wires[i],true);});
   else if(op=="decode_few")m=measure([&](size_t i){return codec->decode(wires[i],false);});
   else m=measure([&](size_t i){codec->encode(events[i],out);return codec->decode(out,true);});
   std::cout<<codec->name()<<","<<bytes<<",random,"<<op<<","<<repeat<<","<<double(total)/corpus_n<<","<<m.iterations<<","<<std::setprecision(9)<<m.ns<<","<<m.checksum<<"\n";
  }
  std::cerr<<"done "<<codec->name()<<" payload="<<bytes<<"\n";
 }
 std::cerr<<"validation=passed\n";return 0;
 } catch(const std::exception&e) {std::cerr<<"ERROR: "<<e.what()<<"\n";return 2;}
}
