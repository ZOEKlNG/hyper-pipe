#pragma once
#include "log.pb.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace handoff {
constexpr size_t CORPUS_N=4096,MAX_TAIL=1024,MAX_WIRE=1280;
enum class Shape{Flat,Nested};
inline const char* shape_name(Shape s){return s==Shape::Flat?"flat":"nested_http";}
struct Header{uint64_t id,seq,timestamp,value0,value1;uint32_t producer,stream,type,flags;uint16_t header_len,version;uint32_t payload_len;};
static_assert(sizeof(Header)==64);
struct ByteSpan{const char*data=nullptr;uint32_t size=0;};
struct SourceView{Header h{};std::array<ByteSpan,3>parts{};ByteSpan flat{};};
struct Packet{std::string bytes;};
struct Slice{uint32_t offset=0,len=0;};
struct alignas(64) NativeSlot{Header h;std::array<Slice,3>slices;std::array<char,MAX_TAIL>tail;};
inline std::vector<Packet> make_corpus(size_t tail_bytes,size_t count=CORPUS_N){
 if(tail_bytes>MAX_TAIL)throw std::invalid_argument("tail exceeds slot");
 std::mt19937_64 rng(0x61aba1+tail_bytes);std::vector<Packet>out;out.reserve(count);
 for(size_t i=0;i<count;i++){
  Header h{rng(),uint64_t(i+1),1770000000000000000ULL+i*17,rng(),rng(),uint32_t(1+i%32),uint32_t(1+i%64),uint32_t(200+i%7),uint32_t(i%16),64,1,uint32_t(tail_bytes)};
  uint32_t lens[3]={uint32_t(tail_bytes/2),uint32_t(tail_bytes/4),uint32_t(tail_bytes-tail_bytes/2-tail_bytes/4)};
  Packet p;p.bytes.resize(144+tail_bytes);std::memcpy(p.bytes.data()+64,&h,64);std::memcpy(p.bytes.data()+128,lens,12);
  for(size_t j=0;j<tail_bytes;j++)p.bytes[144+j]=char(rng());out.push_back(std::move(p));
 }return out;
}
// Simulates extraction of already located scalar fields and three byte spans from
// immutable packet storage. It does not model real Ethernet/HTTP parsing or DPDK.
inline SourceView extract(const Packet&p){
 SourceView v;std::memcpy(&v.h,p.bytes.data()+64,64);uint32_t lens[3];std::memcpy(lens,p.bytes.data()+128,12);
 uint32_t pos=0;for(size_t i=0;i<3;i++){v.parts[i]={p.bytes.data()+144+pos,lens[i]};pos+=lens[i];}v.flat={p.bytes.data()+144,pos};return v;
}
inline void fill_slot(NativeSlot&slot,const SourceView&s){
 slot.h=s.h;uint32_t pos=0;for(size_t i=0;i<3;i++){slot.slices[i]={pos,s.parts[i].size};if(s.parts[i].size)std::memcpy(slot.tail.data()+pos,s.parts[i].data,s.parts[i].size);pos+=s.parts[i].size;}slot.h.payload_len=pos;
}
inline SourceView view_slot(const NativeSlot&slot){
 SourceView v;v.h=slot.h;for(size_t i=0;i<3;i++)v.parts[i]={slot.tail.data()+slot.slices[i].offset,slot.slices[i].len};v.flat={slot.tail.data(),slot.h.payload_len};return v;
}
struct PBState{
 handoffpb::FixedEvent flat;
 handoffpb::GatewayLogEvent nested;
 PBState(){nested.mutable_common();nested.mutable_http();}
};
inline void fill_pb(PBState&state,Shape shape,const SourceView&s){
 const auto&h=s.h;
 if(shape==Shape::Flat){auto&m=state.flat;m.set_id(h.id);m.set_seq(h.seq);m.set_timestamp(h.timestamp);m.set_value0(h.value0);m.set_value1(h.value1);m.set_producer(h.producer);m.set_stream(h.stream);m.set_type(h.type);m.set_flags(h.flags);m.set_payload(s.flat.data,s.flat.size);}
 else{auto*c=state.nested.mutable_common();auto*m=state.nested.mutable_http();c->set_id(h.id);c->set_seq(h.seq);c->set_timestamp(h.timestamp);c->set_value0(h.value0);c->set_value1(h.value1);c->set_producer(h.producer);c->set_stream(h.stream);m->set_status(h.type);m->set_method(h.flags);m->set_url(s.parts[0].data,s.parts[0].size);m->set_host(s.parts[1].data,s.parts[1].size);m->set_cookie(s.parts[2].data,s.parts[2].size);}
}
inline size_t serialize_pb(PBState&state,Shape shape,uint8_t*out,size_t capacity){
 const google::protobuf::MessageLite&m=shape==Shape::Flat?static_cast<const google::protobuf::MessageLite&>(state.flat):static_cast<const google::protobuf::MessageLite&>(state.nested);
 size_t n=m.ByteSizeLong();if(n>capacity)throw std::runtime_error("wire exceeds capacity");uint8_t*end=m.SerializeWithCachedSizesToArray(out);if(end!=out+n)throw std::runtime_error("wire length mismatch");return n;
}
inline size_t encode_pb(PBState&state,Shape shape,const SourceView&s,uint8_t*out,size_t capacity=MAX_WIRE){fill_pb(state,shape,s);return serialize_pb(state,shape,out,capacity);}
inline uint64_t wire_checksum(const uint8_t*p,size_t n){return n+(n?uint64_t(p[0])+p[n-1]:0);}
inline void escape(const void*p){asm volatile("" : : "g"(p) : "memory");}
inline bool verify_wire(Shape shape,std::string_view wire,const SourceView&s){
 const auto&h=s.h;
 if(shape==Shape::Flat){handoffpb::FixedEvent m;if(!m.ParseFromArray(wire.data(),int(wire.size())))return false;return m.id()==h.id&&m.seq()==h.seq&&m.timestamp()==h.timestamp&&m.value0()==h.value0&&m.value1()==h.value1&&m.producer()==h.producer&&m.stream()==h.stream&&m.type()==h.type&&m.flags()==h.flags&&m.payload()==std::string_view(s.flat.data,s.flat.size);}
 handoffpb::GatewayLogEvent event;if(!event.ParseFromArray(wire.data(),int(wire.size()))||!event.has_common()||!event.has_http())return false;const auto&c=event.common();const auto&m=event.http();return c.id()==h.id&&c.seq()==h.seq&&c.timestamp()==h.timestamp&&c.value0()==h.value0&&c.value1()==h.value1&&c.producer()==h.producer&&c.stream()==h.stream&&m.status()==h.type&&m.method()==h.flags&&m.url()==std::string_view(s.parts[0].data,s.parts[0].size)&&m.host()==std::string_view(s.parts[1].data,s.parts[1].size)&&m.cookie()==std::string_view(s.parts[2].data,s.parts[2].size);
}
} // namespace handoff
