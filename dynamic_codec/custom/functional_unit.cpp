#include "codecs.hpp"
#include "event.pb.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <random>
#include <functional>

using namespace dynbench;
namespace {
size_t checks=0;
void check(bool value,const char*why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&&f,const char*why){bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}check(rejected,why);}
Plan plan(std::initializer_list<unsigned> ids){Plan p;p.name="functional";for(unsigned id:ids){FieldOp op;op.fid=id;op.is_payload=id==10;op.kind=id==10?ScalarKind::Bytes:id<=5?ScalarKind::U64:ScalarKind::U32;op.source_offset=id<10?custom_detail::expected_offset(id):0;op.tag_bytes=custom_detail::make_tag(id,op.is_payload?2:0);op.fixed_tag_bytes=custom_detail::make_tag(id,op.is_payload?2:id<=5?1:5);p.mask|=uint16_t(1)<<(id-1);p.ops.push_back(std::move(op));}validate_custom_plan(p);return p;}
template<class M>void set_message(M&m,const Event&e,const Plan&p){
 if(p.has(1))m.set_id(e.h.id);
 if(p.has(2))m.set_seq(e.h.seq);
 if(p.has(3))m.set_timestamp(e.h.timestamp);
 if(p.has(4))m.set_value0(e.h.value0);
 if(p.has(5))m.set_value1(e.h.value1);
 if(p.has(6))m.set_producer(e.h.producer);
 if(p.has(7))m.set_stream(e.h.stream);
 if(p.has(8))m.set_type(e.h.type);
 if(p.has(9))m.set_flags(e.h.flags);
 if(p.has(10))m.set_payload(e.payload);
}
template<class M>uint64_t message_sum(const M&m,bool few){if(few)return m.id()+m.seq()+m.type();return m.id()+m.seq()+m.timestamp()+m.value0()+m.value1()+m.producer()+m.stream()+m.type()+m.flags()+payload_checksum(m.payload());}
std::array<uint64_t,9> expected_fields(const Event&e,const Plan&p){
 std::array<uint64_t,9> a={e.h.id,e.h.seq,e.h.timestamp,e.h.value0,e.h.value1,e.h.producer,e.h.stream,e.h.type,e.h.flags};
 for(unsigned i=0;i<9;i++)if(!p.has(i+1))a[i]=0;
 return a;
}
void verify_view(const CustomDecodedView&v,const Event&e,const Plan&p){
 auto want=expected_fields(e,p);for(unsigned i=0;i<9;i++)check(v.scalars[i]==want[i],"per-field scalar mismatch");
 check(v.payload==(p.has(10)?std::string_view(e.payload):std::string_view{}),"full payload bytes mismatch");
}
template<class M>void verify_message_fields(const M&m,const Event&e,const Plan&p){
 std::array<uint64_t,9> got={m.id(),m.seq(),m.timestamp(),m.value0(),m.value1(),m.producer(),m.stream(),m.type(),m.flags()};
 auto want=expected_fields(e,p);for(unsigned i=0;i<9;i++)check(got[i]==want[i],"official per-field scalar mismatch");
 check(m.payload()==(p.has(10)?e.payload:std::string{}),"official full payload bytes mismatch");
}
void verify_one(Codec&codec,const Event&e,const Plan&p,std::string&out,bool protobuf=false,bool fixed=false){
 codec.encode(e,p,out);
 verify_view(protobuf?pb_wire_view_checked(out):hybrid_tv_tlv_view_checked(out),e,p);
 for(bool few:{false,true})check(codec.decode(out,few)==expected_checksum(e,p,few),"custom checksum mismatch");
 if(protobuf){
  if(fixed){bench::FixedEvent m;check(m.ParseFromString(out),"official FixedEvent parse failed");verify_message_fields(m,e,p);for(bool few:{false,true})check(message_sum(m,few)==expected_checksum(e,p,few),"official FixedEvent mismatch");}
  else {bench::Event m;check(m.ParseFromString(out),"official Event parse failed");verify_message_fields(m,e,p);for(bool few:{false,true})check(message_sum(m,few)==expected_checksum(e,p,few),"official Event mismatch");}
 }
}
std::string hybrid_scalar(unsigned id,unsigned type,uint64_t value){std::string s(10,'\0');char*p=s.data();custom_detail::write16(p,uint16_t((type<<14)|id));if(type==0)custom_detail::write32(p,uint32_t(value));else custom_detail::write64(p,value);s.resize(p-s.data());return s;}
std::string hybrid_bytes(unsigned id,std::string_view data){std::string s(4+data.size(),'\0');char*p=s.data();custom_detail::write16(p,uint16_t(0x8000|id));custom_detail::write16(p,uint16_t(data.size()));if(!data.empty())std::memcpy(p,data.data(),data.size());return s;}
std::string pb_var(unsigned id,uint64_t value){std::string s=custom_detail::make_tag(id,0);char temp[10];char*p=temp;custom_detail::write_varint(p,value);s.append(temp,p);return s;}
std::string pb_bytes(unsigned id,std::string_view bytes){std::string s=custom_detail::make_tag(id,2);char temp[10];char*p=temp;custom_detail::write_varint(p,bytes.size());s.append(temp,p);s.append(bytes);return s;}
}
int main(){try{
 Plan full=plan({1,2,3,4,5,6,7,8,9,10}),few=plan({1,2,8}),reordered=plan({10,9,1,7}),empty=plan({});
 HybridCodec hybrid;ProtoEmitCodec protobuf(false),fixed(true);std::string out="stale wire contents";
 std::mt19937_64 random(0x610ab);size_t records=0;
 for(size_t n:{0,64,256,1024})for(unsigned i=0;i<128;i++){
  Event e;e.h={random(),uint64_t(i),random(),random(),random(),uint32_t(random()),uint32_t(random()),uint32_t(random()),uint32_t(random()),64,1,uint32_t(n)};e.payload.resize(n);
  for(auto&b:e.payload)b=char(random());
  if(i%7==0){e.h.id=0;e.h.seq=0;e.h.type=0;e.h.flags=0;}if(i%11==0)std::fill(e.payload.begin(),e.payload.end(),char(0xff));
  for(const auto*p:{&full,&few,&reordered,&empty}){verify_one(hybrid,e,*p,out);verify_one(protobuf,e,*p,out,true,false);verify_one(fixed,e,*p,out,true,true);}
  // Official serializers -> custom checked view: both field encodings work.
  bench::Event standard;set_message(standard,e,full);standard.SerializeToString(&out);check(pb_wire_view_checked(out).checksum(false)==expected_checksum(e,full,false),"official varint wire view mismatch");verify_view(pb_wire_view_checked(out),e,full);
  bench::FixedEvent standard_fixed;set_message(standard_fixed,e,full);standard_fixed.SerializeToString(&out);check(pb_wire_view_checked(out).checksum(false)==expected_checksum(e,full,false),"official fixed wire view mismatch");verify_view(pb_wire_view_checked(out),e,full);
  ++records;
 }
 Event edge;edge.h={UINT64_MAX,UINT64_MAX,uint64_t(1)<<63,0,UINT64_MAX,UINT32_MAX,0,UINT32_MAX,0,64,1,3};edge.payload=std::string("\0\xff\x80",3);
 for(const auto*p:{&full,&few,&empty,&full}){verify_one(hybrid,edge,*p,out);verify_one(protobuf,edge,*p,out,true,false);verify_one(fixed,edge,*p,out,true,true);}
 Event zero;zero.h.header_len=64;zero.h.version=1;
 for(auto*c:std::array<Codec*,3>{&hybrid,&protobuf,&fixed}){c->encode(zero,full,out);check(out.empty(),"default fields must be omitted");check(c->decode({},false)==0,"missing defaults mismatch");}
 // Explicit zero/empty values and duplicates use last value, just like singular proto3 fields.
 check(hybrid.decode(hybrid_scalar(1,1,0)+hybrid_bytes(10,{}),false)==0,"Hybrid explicit default mismatch");
 check(protobuf.decode(pb_var(1,0)+pb_bytes(10,{}),false)==0,"PB explicit default mismatch");
 check(hybrid.decode(hybrid_scalar(1,1,99)+hybrid_scalar(1,1,0),false)==0,"Hybrid duplicate last-wins mismatch");
 check(protobuf.decode(pb_var(1,99)+pb_var(1,0),false)==0,"PB duplicate last-wins mismatch");
 check(protobuf.decode(pb_bytes(10,"old")+pb_bytes(10,"new!"),false)==payload_checksum("new!"),"PB bytes duplicate last-wins mismatch");
 // Unknown fields must be validated/skipped, and must not change known-field checksum.
 check(hybrid.decode(hybrid_scalar(1,1,7)+hybrid_scalar(16383,1,UINT64_MAX)+hybrid_bytes(91,"abc"),false)==7,"Hybrid unknown skip mismatch");
 std::string unknown_fixed64=custom_detail::make_tag(40000,1);unknown_fixed64.append(8,char(0xff));
 std::string unknown_fixed32=custom_detail::make_tag(40001,5);unknown_fixed32.append(4,char(0xff));
 check(protobuf.decode(pb_var(1,7)+pb_var(90,UINT64_MAX)+pb_bytes(91,"abc")+unknown_fixed64+unknown_fixed32,false)==7,"PB unknown skip mismatch");
 // Payload is genuinely a borrowed view of the input bytes, with no payload allocation/copy.
 hybrid.encode(edge,full,out);auto hv=hybrid_tv_tlv_view_checked(out);check(hv.payload.size()==3&&hv.payload.data()>=out.data()&&hv.payload.data()+3<=out.data()+out.size(),"Hybrid payload is not a wire view");
 protobuf.encode(edge,full,out);auto pv=pb_wire_view_checked(out);check(pv.payload.size()==3&&pv.payload.data()>=out.data()&&pv.payload.data()+3<=out.data()+out.size(),"PB payload is not a wire view");
 // Whole single fields truncated at every interior byte must fail (field-boundary prefixes may be valid messages).
 for(const auto&wire:std::array<std::string,3>{hybrid_scalar(1,1,UINT64_MAX),hybrid_scalar(6,0,UINT32_MAX),hybrid_bytes(10,"abc")})for(size_t n=1;n<wire.size();n++)rejects([&]{hybrid.decode(std::string_view(wire).substr(0,n),false);},"Hybrid accepted truncated field");
 for(const auto&wire:std::array<std::string,4>{pb_var(1,UINT64_MAX),pb_bytes(10,"abc"),unknown_fixed64,unknown_fixed32})for(size_t n=1;n<wire.size();n++)rejects([&]{protobuf.decode(std::string_view(wire).substr(0,n),false);},"PB accepted truncated field");
 rejects([&]{hybrid.decode(std::string("\0\0",2),false);},"Hybrid accepted fid0");
 rejects([&]{hybrid.decode(std::string("\1\xc0",2),false);},"Hybrid accepted reserved type3");
 rejects([&]{hybrid.decode(hybrid_scalar(1,0,3),false);},"Hybrid accepted wrong known width");
 rejects([&]{protobuf.decode(std::string("\0",1),false);},"PB accepted fid0");
 rejects([&]{protobuf.decode(std::string("\x0b",1),false);},"PB accepted unsupported group");
 rejects([&]{protobuf.decode(std::string("\x0e",1),false);},"PB accepted invalid wire type6");
 rejects([&]{protobuf.decode(pb_var(6,uint64_t(UINT32_MAX)+1),false);},"PB silently truncated u32");
 rejects([&]{protobuf.decode(pb_bytes(1,"x"),false);},"PB accepted wrong known wire type");
 std::string wrong_fixed=custom_detail::make_tag(1,1);wrong_fixed.append(8,char(0));
 rejects([&]{protobuf.decode(wrong_fixed,false);},"varint-schema decoder accepted fixed64");
 rejects([&]{fixed.decode(pb_var(1,5),false);},"fixed-schema decoder accepted varint");
 std::string overflow=custom_detail::make_tag(1,0);overflow.append(9,char(0xff));overflow.push_back(char(2));rejects([&]{protobuf.decode(overflow,false);},"PB accepted uint64 overflow");
 std::string unterminated=custom_detail::make_tag(1,0);unterminated.append(10,char(0x80));rejects([&]{protobuf.decode(unterminated,false);},"PB accepted unterminated varint");
 std::string oversized_tag(5,char(0xff));oversized_tag.push_back(char(1));rejects([&]{protobuf.decode(oversized_tag,false);},"PB accepted oversized tag");
 std::string truncated_unknown=pb_bytes(50000,"hello");truncated_unknown.pop_back();rejects([&]{protobuf.decode(truncated_unknown,false);},"PB skipped truncated unknown bytes");
 // Size limits reject rather than silently narrowing Hybrid's u16 byte count.
 Event long_event;long_event.payload=std::string(65535,char(0xff));long_event.h.header_len=64;long_event.h.version=1;
 Plan payload_only=plan({10});hybrid.encode(long_event,payload_only,out);check(hybrid.decode(out,false)==payload_checksum(long_event.payload),"Hybrid 65535 failed");
 long_event.payload.push_back('x');rejects([&]{hybrid.encode(long_event,payload_only,out);},"Hybrid truncated 65536 to u16");
 std::string giant(custom_detail::max_frame_bytes+1,'x');rejects([&]{hybrid.decode(giant,false);},"Hybrid accepted oversized frame");rejects([&]{protobuf.decode(giant,false);},"PB accepted oversized frame");
 auto bad=full;bad.ops.push_back(bad.ops[0]);rejects([&]{validate_custom_plan(bad);},"duplicate plan field accepted");
 bad=few;bad.ops[0].source_offset=99;rejects([&]{validate_custom_plan(bad);},"bad source offset accepted");
 bad=few;bad.ops[0].tag_bytes="bad";rejects([&]{validate_custom_plan(bad);},"bad compiled tag accepted");
 bad=few;bad.mask=0;rejects([&]{validate_custom_plan(bad);},"bad plan mask accepted");
 std::cout<<"PASS custom codecs; checks="<<checks<<" corpus_records="<<records<<" official_protobuf="<<GOOGLE_PROTOBUF_VERSION<<"\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
