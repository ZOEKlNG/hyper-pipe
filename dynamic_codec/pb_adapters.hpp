#pragma once
#include "common.hpp"
#include "plan_compiler.hpp"
#include "../codec/event.pb.h"
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/descriptor.pb.h>
namespace dynbench {
template<class PB,bool StaticFull> class GeneratedPB final:public Codec {
 PB msg_;
public:
 const char*name()const override{
  if constexpr(std::is_same_v<PB,bench::FixedEvent>)return StaticFull?"pb_generated_fixed_static_full":"pb_generated_fixed_plan_selective";
  return StaticFull?"pb_generated_varint_static_full":"pb_generated_varint_plan_selective";
 }
 __attribute__((noinline)) size_t encode(const Event&e,const Plan&p,std::string&out)override{
  msg_.Clear();
  if constexpr(StaticFull){
   msg_.set_id(e.h.id);msg_.set_seq(e.h.seq);msg_.set_timestamp(e.h.timestamp);msg_.set_value0(e.h.value0);msg_.set_value1(e.h.value1);
   msg_.set_producer(e.h.producer);msg_.set_stream(e.h.stream);msg_.set_type(e.h.type);msg_.set_flags(e.h.flags);msg_.set_payload(e.payload);
  }else for(const auto&op:p.ops){switch(op.fid){
   case 1:msg_.set_id(e.h.id);break;case 2:msg_.set_seq(e.h.seq);break;case 3:msg_.set_timestamp(e.h.timestamp);break;
   case 4:msg_.set_value0(e.h.value0);break;case 5:msg_.set_value1(e.h.value1);break;case 6:msg_.set_producer(e.h.producer);break;
   case 7:msg_.set_stream(e.h.stream);break;case 8:msg_.set_type(e.h.type);break;case 9:msg_.set_flags(e.h.flags);break;
   case 10:msg_.set_payload(e.payload);break;default:throw std::invalid_argument("bad plan fid");
  }}
  if(!msg_.SerializeToString(&out))throw std::runtime_error("protobuf serialize failed");escape(out.data());return out.size();
 }
 __attribute__((noinline)) uint64_t decode(std::string_view wire,bool few)override{
  if(!msg_.ParseFromArray(wire.data(),int(wire.size())))throw std::runtime_error("protobuf parse failed");
  uint64_t s=msg_.id()+msg_.seq()+msg_.type();if(few)return s;
  return s+msg_.timestamp()+msg_.value0()+msg_.value1()+msg_.producer()+msg_.stream()+msg_.flags()+payload_checksum(msg_.payload());
 }
};
class ReflectionPB final:public Codec {
 bool fixed_;google::protobuf::DescriptorPool pool_;
 google::protobuf::DynamicMessageFactory factory_{&pool_};
 const google::protobuf::Descriptor*desc_=nullptr;
 const google::protobuf::Reflection* reflection_=nullptr;
 std::unique_ptr<google::protobuf::Message> msg_;
 const google::protobuf::FieldDescriptor* fields_[11]{};
public:
 explicit ReflectionPB(bool fixed):fixed_(fixed){
  using FDP=google::protobuf::FieldDescriptorProto;
  google::protobuf::FileDescriptorProto file;file.set_name(fixed?"runtime_fixed.proto":"runtime_varint.proto");file.set_package("runtimebench");file.set_syntax("proto3");
  auto*m=file.add_message_type();m->set_name("Event");
  for(const auto&meta:registry){auto*f=m->add_field();f->set_name(meta.name);f->set_number(meta.fid);f->set_label(FDP::LABEL_OPTIONAL);
   f->set_type(meta.kind==ScalarKind::Bytes?FDP::TYPE_BYTES:meta.kind==ScalarKind::U64?(fixed?FDP::TYPE_FIXED64:FDP::TYPE_UINT64):(fixed?FDP::TYPE_FIXED32:FDP::TYPE_UINT32));}
  const auto*fd=pool_.BuildFile(file);if(!fd)throw std::runtime_error("BuildFile failed");desc_=fd->message_type(0);
  const auto* prototype=factory_.GetPrototype(desc_);if(!prototype)throw std::runtime_error("GetPrototype failed");msg_.reset(prototype->New());reflection_=msg_->GetReflection();
  for(int fid=1;fid<=10;fid++)fields_[fid]=desc_->FindFieldByNumber(fid);
 }
 const char*name()const override{return fixed_?"pb_dynamicmessage_fixed_reflection_reuse":"pb_dynamicmessage_varint_reflection_reuse";}
 __attribute__((noinline)) size_t encode(const Event&e,const Plan&p,std::string&out)override{
  msg_->Clear();for(const auto&op:p.ops){auto*f=fields_[op.fid];if(op.is_payload)reflection_->SetString(msg_.get(),f,e.payload);
   else if(op.kind==ScalarKind::U32)reflection_->SetUInt32(msg_.get(),f,uint32_t(scalar_value(e,op)));
   else reflection_->SetUInt64(msg_.get(),f,scalar_value(e,op));}
  if(!msg_->SerializeToString(&out))throw std::runtime_error("reflection serialize failed");escape(out.data());return out.size();
 }
 __attribute__((noinline)) uint64_t decode(std::string_view wire,bool few)override{
  if(!msg_->ParseFromArray(wire.data(),int(wire.size())))throw std::runtime_error("reflection parse failed");
  uint64_t s=reflection_->GetUInt64(*msg_,fields_[1])+reflection_->GetUInt64(*msg_,fields_[2])+reflection_->GetUInt32(*msg_,fields_[8]);if(few)return s;
  for(int fid:{3,4,5})s+=reflection_->GetUInt64(*msg_,fields_[fid]);for(int fid:{6,7,9})s+=reflection_->GetUInt32(*msg_,fields_[fid]);
  std::string scratch;const auto& payload=reflection_->GetStringReference(*msg_,fields_[10],&scratch);return s+payload_checksum(payload);
 }
};
}
