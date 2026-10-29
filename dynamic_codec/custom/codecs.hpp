#pragma once
#include "../common.hpp"
#include <array>
#include <limits>
#include <utility>

// New handwritten benchmark prototypes, not a general-purpose codec library.
// Configuration/Plan construction must validate field selection before timing.
namespace dynbench {
namespace custom_detail {
inline constexpr size_t max_frame_bytes = 1u << 20;
inline constexpr size_t hybrid_max_bytes = 65535;
[[noreturn]] inline void invalid(const char* why) { throw std::invalid_argument(why); }
inline uint16_t read16(const char* p) {
 return uint16_t(uint8_t(p[0])) | uint16_t(uint16_t(uint8_t(p[1])) << 8);
}
inline uint32_t read32(const char* p) {
 uint32_t value=0;for(unsigned i=0;i<4;i++)value|=uint32_t(uint8_t(p[i]))<<(8*i);return value;
}
inline uint64_t read64(const char* p) {
 uint64_t value=0;for(unsigned i=0;i<8;i++)value|=uint64_t(uint8_t(p[i]))<<(8*i);return value;
}
inline void write16(char*& p,uint16_t v) { *p++=char(v);*p++=char(v>>8); }
inline void write32(char*& p,uint32_t v) { for(unsigned i=0;i<4;i++)*p++=char(v>>(8*i)); }
inline void write64(char*& p,uint64_t v) { for(unsigned i=0;i<8;i++)*p++=char(v>>(8*i)); }
inline void write_varint(char*&p,uint64_t v) {
 while(v>=128){*p++=char(uint8_t(v)|128);v>>=7;}*p++=char(v);
}
inline uint64_t read_varint(std::string_view w,size_t&pos,unsigned max_bytes=10) {
 uint64_t value=0;
 for(unsigned i=0;i<max_bytes;i++) {
  if(pos==w.size())invalid("truncated varint");
  uint8_t b=uint8_t(w[pos++]);
  if(i==9 && b>1)invalid("uint64 varint overflow");
  value|=uint64_t(b&127)<<(7*i);
  if(!(b&128))return value;
 }
 invalid("unterminated or oversized varint");
}
inline size_t scalar_width(ScalarKind kind) {
 if(kind==ScalarKind::U32||kind==ScalarKind::Fixed32)return 4;
 if(kind==ScalarKind::U64||kind==ScalarKind::Fixed64)return 8;
 invalid("bytes is not a scalar");
}
inline uint64_t input_scalar(const Event&e,const FieldOp&op) {
 size_t n=scalar_width(op.kind);
 if(op.source_offset>sizeof(Header)-n)invalid("invalid scalar source offset");
 if(n==4){uint32_t value;std::memcpy(&value,reinterpret_cast<const char*>(&e.h)+op.source_offset,4);return value;}
 uint64_t value;std::memcpy(&value,reinterpret_cast<const char*>(&e.h)+op.source_offset,8);return value;
}
inline unsigned pb_wire(const FieldOp&op,bool fixed) {
 if(op.is_payload)return 2;
 if(fixed||op.kind==ScalarKind::Fixed32||op.kind==ScalarKind::Fixed64)return scalar_width(op.kind)==4?5:1;
 return 0;
}
inline std::string make_tag(uint32_t fid,unsigned wire) {
 char b[5];char*p=b;write_varint(p,(uint64_t(fid)<<3)|wire);return std::string(b,p);
}
inline size_t expected_offset(uint32_t fid) {
 constexpr std::array<size_t,9> offsets={offsetof(Header,id),offsetof(Header,seq),offsetof(Header,timestamp),offsetof(Header,value0),offsetof(Header,value1),offsetof(Header,producer),offsetof(Header,stream),offsetof(Header,type),offsetof(Header,flags)};
 if(fid<1||fid>9)invalid("not a scalar field id");
 return offsets[fid-1];
}
inline void check_size(std::string_view w) {if(w.size()>max_frame_bytes)invalid("frame exceeds 1 MiB limit");}
inline size_t prepare_output(const Event&e,const Plan&p,std::string&out,bool hybrid) {
 if(p.ops.size()>10)invalid("too many operations in Event subset plan");
 size_t n=p.ops.size()*16;
 // This upper bound also remains safe if a C++ caller constructed a duplicate bytes op.
 for(const auto&op:p.ops)if(op.is_payload){
  if(e.payload.size()>(hybrid?hybrid_max_bytes:max_frame_bytes))invalid("payload exceeds codec limit");
  if(e.payload.size()>max_frame_bytes-n)invalid("encoded upper bound exceeds limit");
  n+=e.payload.size();
 }
 out.resize(n);return n;
}
} // namespace custom_detail

// Call once while compiling trusted runtime configuration, outside the hot loop.
inline void validate_custom_plan(const Plan&p) {
 if(p.ops.size()>10)custom_detail::invalid("Event plan has more than 10 fields");
 uint16_t mask=0;
 for(const auto&op:p.ops) {
  if(op.fid<1||op.fid>10)custom_detail::invalid("Event plan field id outside 1..10");
  uint16_t bit=uint16_t(1)<<(op.fid-1);
  if(mask&bit)custom_detail::invalid("duplicate field in Event plan");
  mask|=bit;
  if(op.fid==10){if(!op.is_payload||op.kind!=ScalarKind::Bytes)custom_detail::invalid("field10 must be payload bytes");}
  else {
   if(op.is_payload||op.kind==ScalarKind::Bytes)custom_detail::invalid("scalar field cannot use bytes getter");
   if(custom_detail::scalar_width(op.kind)!=(op.fid<=5?8u:4u))custom_detail::invalid("scalar width disagrees with Event schema");
   if(op.source_offset!=custom_detail::expected_offset(op.fid))custom_detail::invalid("source offset disagrees with Event schema");
  }
  if(op.tag_bytes!=custom_detail::make_tag(op.fid,custom_detail::pb_wire(op,false))||op.fixed_tag_bytes!=custom_detail::make_tag(op.fid,custom_detail::pb_wire(op,true)))custom_detail::invalid("precompiled Protobuf tag disagrees with field op");
 }
 if(mask!=p.mask)custom_detail::invalid("plan mask disagrees with field operations");
}

// Scalars are materialized; payload remains a borrowed view into the input wire.
// No allocation or payload copy occurs in either checked view decoder.
struct CustomDecodedView {
 std::array<uint64_t,9> scalars{};
 std::string_view payload{};
 uint64_t checksum(bool few)const {
  if(few)return scalars[0]+scalars[1]+scalars[7];
  uint64_t sum=payload_checksum(payload);for(auto value:scalars)sum+=value;return sum;
 }
};

inline CustomDecodedView hybrid_tv_tlv_view_checked(std::string_view w) {
 using namespace custom_detail;check_size(w);CustomDecodedView result;size_t pos=0;
 while(pos<w.size()) {
  if(w.size()-pos<2)invalid("truncated Hybrid tag");
  uint16_t tag=read16(w.data()+pos);pos+=2;uint32_t fid=tag&0x3fff;unsigned type=tag>>14;
  if(fid==0)invalid("Hybrid field id zero is invalid");
  if(type==3)invalid("Hybrid type3 is reserved");
  uint64_t value=0;std::string_view payload;
  if(type==0){if(w.size()-pos<4)invalid("truncated Hybrid u32");value=read32(w.data()+pos);pos+=4;}
  else if(type==1){if(w.size()-pos<8)invalid("truncated Hybrid u64");value=read64(w.data()+pos);pos+=8;}
  else {
   if(w.size()-pos<2)invalid("truncated Hybrid bytes length");
   size_t n=read16(w.data()+pos);pos+=2;
   if(n>w.size()-pos)invalid("truncated Hybrid bytes");
   payload=w.substr(pos,n);pos+=n;
  }
  if(fid<=5){if(type!=1)invalid("Hybrid known u64 wire type mismatch");result.scalars[fid-1]=value;}
  else if(fid<=9){if(type!=0)invalid("Hybrid known u32 wire type mismatch");result.scalars[fid-1]=value;}
  else if(fid==10){if(type!=2)invalid("Hybrid known bytes wire type mismatch");result.payload=payload;}
  // Unknown fields are validated and skipped, never added to the checksum.
 }
 return result;
}

// A checked zero-copy VIEW for this benchmark's scalar/bytes Protobuf subset.
// It deliberately does not implement groups, nested messages, packed repeated,
// unknown-field preservation, signed types, or a general runtime descriptor API.
enum class PBScalarWire { Varint, Fixed, Either };
inline CustomDecodedView pb_wire_view_checked(std::string_view w,PBScalarWire mode=PBScalarWire::Either) {
 using namespace custom_detail;check_size(w);CustomDecodedView result;size_t pos=0;
 while(pos<w.size()) {
  uint64_t tag=read_varint(w,pos,5);
  if(tag>std::numeric_limits<uint32_t>::max())invalid("Protobuf tag overflow");
  uint32_t fid=uint32_t(tag>>3);unsigned type=unsigned(tag&7);
  if(fid==0||fid>0x1fffffff)invalid("Protobuf field id out of range");
  uint64_t value=0;std::string_view payload;
  if(type==0)value=read_varint(w,pos);
  else if(type==1){if(w.size()-pos<8)invalid("truncated Protobuf fixed64");value=read64(w.data()+pos);pos+=8;}
  else if(type==2){uint64_t n=read_varint(w,pos);if(n>w.size()-pos)invalid("truncated Protobuf bytes");payload=w.substr(pos,size_t(n));pos+=size_t(n);}
  else if(type==5){if(w.size()-pos<4)invalid("truncated Protobuf fixed32");value=read32(w.data()+pos);pos+=4;}
  else invalid("unsupported Protobuf wire type in checked subset");
  if(fid<=5){if(type!=0&&type!=1)invalid("Protobuf known u64 wire type mismatch");if((mode==PBScalarWire::Varint&&type!=0)||(mode==PBScalarWire::Fixed&&type!=1))invalid("Protobuf known u64 schema wire mismatch");result.scalars[fid-1]=value;}
  else if(fid<=9){if(type!=0&&type!=5)invalid("Protobuf known u32 wire type mismatch");if((mode==PBScalarWire::Varint&&type!=0)||(mode==PBScalarWire::Fixed&&type!=5))invalid("Protobuf known u32 schema wire mismatch");if(value>std::numeric_limits<uint32_t>::max())invalid("Protobuf u32 overflow; truncation is not allowed");result.scalars[fid-1]=value;}
  else if(fid==10){if(type!=2)invalid("Protobuf known bytes wire type mismatch");result.payload=payload;}
 }
 return result;
}

class HybridCodec final:public Codec {
public:
 static constexpr const char* decoder_name="hybrid_tv_tlv_view_checked";
 const char* name()const override{return "custom_hybrid_tv_tlv";}
 size_t encode(const Event&e,const Plan&p,std::string&out)override {
  using namespace custom_detail;prepare_output(e,p,out,true);char*start=out.data();char*cursor=start;
  for(const auto&op:p.ops) {
   if(op.fid==0||op.fid>0x3fff)invalid("Hybrid plan field id out of range");
   if(op.is_payload){if(e.payload.empty())continue;write16(cursor,uint16_t(0x8000|op.fid));write16(cursor,uint16_t(e.payload.size()));std::memcpy(cursor,e.payload.data(),e.payload.size());cursor+=e.payload.size();}
   else {uint64_t value=input_scalar(e,op);if(value==0)continue;if(scalar_width(op.kind)==4){write16(cursor,uint16_t(op.fid));write32(cursor,uint32_t(value));}else{write16(cursor,uint16_t(0x4000|op.fid));write64(cursor,value);}}
  }
  size_t n=size_t(cursor-start);out.resize(n);escape(out.data());return n;
 }
 uint64_t decode(std::string_view w,bool few)override{return hybrid_tv_tlv_view_checked(w).checksum(few);}
};

class ProtoEmitCodec final:public Codec {
 bool fixed_;
public:
 explicit ProtoEmitCodec(bool fixed=false):fixed_(fixed){}
 static constexpr const char* decoder_name="pb_wire_view_checked";
 const char* name()const override{return fixed_?"custom_proto_emit_op_fixed":"custom_proto_emit_op_varint";}
 size_t encode(const Event&e,const Plan&p,std::string&out)override {
  using namespace custom_detail;prepare_output(e,p,out,false);char*start=out.data();char*cursor=start;
  for(const auto&op:p.ops) {
   if(op.fid==0||op.fid>0x1fffffff)invalid("Protobuf plan field id out of range");
   if(!fixed_&&(op.kind==ScalarKind::Fixed32||op.kind==ScalarKind::Fixed64))invalid("ProtoEmit varint mode requires the logical U32/U64 plan");
   uint64_t value=op.is_payload?0:input_scalar(e,op);if(op.is_payload?e.payload.empty():value==0)continue;
   const auto&tag=fixed_?op.fixed_tag_bytes:op.tag_bytes;
   if(tag.empty()||tag.size()>5)invalid("invalid precompiled Protobuf tag size");
   std::memcpy(cursor,tag.data(),tag.size());cursor+=tag.size();
   if(op.is_payload){write_varint(cursor,e.payload.size());std::memcpy(cursor,e.payload.data(),e.payload.size());cursor+=e.payload.size();}
   else {unsigned wire=pb_wire(op,fixed_);if(wire==0)write_varint(cursor,value);else if(wire==5)write32(cursor,uint32_t(value));else write64(cursor,value);}
  }
  size_t n=size_t(cursor-start);out.resize(n);escape(out.data());return n;
 }
 uint64_t decode(std::string_view w,bool few)override{return pb_wire_view_checked(w,fixed_?PBScalarWire::Fixed:PBScalarWire::Varint).checksum(few);}
};
} // namespace dynbench
