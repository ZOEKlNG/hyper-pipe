#include "adapter.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>
#include "upb/base/descriptor_constants.h"
#include "upb/base/status.h"
#include "upb/base/string_view.h"
#include "upb/mem/arena.h"
#include "upb/message/message.h"
#include "upb/message/accessors.h"
#include "upb/mini_descriptor/decode.h"
#include "upb/mini_descriptor/internal/encode.h"
#include "upb/mini_descriptor/internal/modifiers.h"
#include "upb/mini_table/field.h"
#include "upb/mini_table/message.h"
#include "upb/wire/decode.h"
#include "upb/wire/encode.h"

namespace dynbench {
namespace {
constexpr size_t kScratchBytes=65536;
constexpr size_t kMaxPayloadBytes=8192;
// Explicitly reject growth. In v29.3, a tagged null allocator's exhaustion
// path can reach upb_malloc(nullptr,...); this official callback avoids it.
void* reject_arena_growth(upb_alloc*,void*,size_t,size_t){return nullptr;}
upb_alloc bounded_allocator{reject_arena_growth};

struct ArenaScope {
  upb_Arena* arena;
  ArenaScope(void* scratch,size_t n,bool heap):arena(heap?upb_Arena_New():upb_Arena_Init(scratch,n,&bounded_allocator)) {
    if(!arena)throw std::bad_alloc();
  }
  ~ArenaScope(){upb_Arena_Free(arena);}
  ArenaScope(const ArenaScope&)=delete;
};

class UpbCodec final:public Codec {
  UpbOptions options_;
  std::string name_;
  upb_Arena* schema_arena_=nullptr;
  upb_MiniTable* table_=nullptr;
  std::array<const upb_MiniTableField*,10> fields_{};
  alignas(std::max_align_t) std::array<std::byte,kScratchBytes> scratch_;

  void make_schema() {
    schema_arena_=upb_Arena_New();
    if(!schema_arena_)throw std::bad_alloc();
    char buffer[512];upb_MtDataEncoder encoder{};encoder.end=buffer+sizeof(buffer);
    char* end=upb_MtDataEncoder_StartMessage(&encoder,buffer,0);
    if(!end)throw std::runtime_error("upb MiniDescriptor StartMessage failed");
    for(uint32_t fid=1;fid<=10;fid++) {
      const upb_FieldType kind=fid<=5?(options_.fixed?kUpb_FieldType_Fixed64:kUpb_FieldType_UInt64):fid<=9?(options_.fixed?kUpb_FieldType_Fixed32:kUpb_FieldType_UInt32):kUpb_FieldType_Bytes;
      end=upb_MtDataEncoder_PutField(&encoder,end,kind,fid,kUpb_FieldModifier_IsProto3Singular);
      if(!end)throw std::runtime_error("upb MiniDescriptor PutField failed");
    }
    upb_Status status;upb_Status_Clear(&status);
    table_=upb_MiniTable_Build(buffer,static_cast<size_t>(end-buffer),schema_arena_,&status);
    if(!table_)throw std::runtime_error(std::string("upb runtime MiniTable build: ")+upb_Status_ErrorMessage(&status));
    for(uint32_t fid=1;fid<=10;fid++) {
      fields_[fid-1]=upb_MiniTable_FindFieldByNumber(table_,fid);
      if(!fields_[fid-1]||upb_MiniTableField_Number(fields_[fid-1])!=fid)throw std::runtime_error("upb MiniTable field lookup failed");
    }
  }
  upb_Message* new_message(upb_Arena* arena)const {
    auto* msg=upb_Message_New(table_,arena);if(!msg)throw std::bad_alloc();return msg;
  }
  void fill(upb_Message* msg,upb_Arena* arena,const Event& event,const Plan& plan)const {
    if(event.payload.size()>kMaxPayloadBytes)throw std::runtime_error("upb adapter max payload is 8192 bytes");
    for(const auto& op:plan.ops) {
      if(op.fid<1||op.fid>10)throw std::runtime_error("invalid upb field plan");
      const auto* field=fields_[op.fid-1];bool ok;
      if(op.fid==10) {
        const char* p=event.payload.data();
        if(options_.copy_encode_payload&&!event.payload.empty()) {
          auto* owned=static_cast<char*>(upb_Arena_Malloc(arena,event.payload.size()));
          if(!owned)throw std::bad_alloc();
          std::memcpy(owned,p,event.payload.size());p=owned;
        }
        ok=upb_Message_SetString(msg,field,upb_StringView_FromDataAndSize(p,event.payload.size()),arena);
      } else if(op.fid<=5)ok=upb_Message_SetUInt64(msg,field,scalar_value(event,op),arena);
      else ok=upb_Message_SetUInt32(msg,field,static_cast<uint32_t>(scalar_value(event,op)),arena);
      if(!ok)throw std::runtime_error("upb field assignment failed");
    }
  }
  upb_Message* parse(std::string_view wire,upb_Arena* arena,bool alias)const {
    auto* msg=new_message(arena);
    int opts=alias?kUpb_DecodeOption_AliasString:0;
    const auto status=upb_Decode(wire.data(),wire.size(),msg,table_,nullptr,opts,arena);
    if(status!=kUpb_DecodeStatus_Ok)throw std::runtime_error(std::string("upb decode status: ")+std::to_string(static_cast<int>(status)));
    return msg;
  }
  uint64_t consume(const upb_Message* msg,bool few)const {
    uint64_t sum=0;
    if(few) {
      return upb_Message_GetUInt64(msg,fields_[0],0)+upb_Message_GetUInt64(msg,fields_[1],0)+upb_Message_GetUInt32(msg,fields_[7],0);
    }
    for(size_t i=0;i<5;i++)sum+=upb_Message_GetUInt64(msg,fields_[i],0);
    for(size_t i=5;i<9;i++)sum+=upb_Message_GetUInt32(msg,fields_[i],0);
    const auto payload=upb_Message_GetString(msg,fields_[9],upb_StringView_FromDataAndSize(nullptr,0));
    if(payload.size&&payload.data==nullptr)throw std::runtime_error("upb invalid payload view");
    return sum+payload_checksum(std::string_view(payload.size?payload.data:"",payload.size));
  }
public:
  explicit UpbCodec(UpbOptions options):options_(options) {
    name_="upb_v29_3_dynamic_minitable_";
    name_+=options.fixed?"fixed":"varint";
    name_+=options.copy_encode_payload?"_copyinput":"_viewinput";
    name_+=options.alias_decode?"_decodealias":"_decodecopy";
    name_+=options.heap_arena?"_heap_arena":"_prealloc_arena";
    try{make_schema();}catch(...){if(schema_arena_)upb_Arena_Free(schema_arena_);throw;}
  }
  ~UpbCodec()override{upb_Arena_Free(schema_arena_);}
  const char* name()const override{return name_.c_str();}
  size_t encode(const Event& event,const Plan& plan,std::string& out)override {
    ArenaScope arena(scratch_.data(),scratch_.size(),options_.heap_arena);
    auto* msg=new_message(arena.arena);fill(msg,arena.arena,event,plan);
    char* data=nullptr;size_t n=0;
    const auto status=upb_Encode(msg,table_,0,arena.arena,&data,&n);
    if(status!=kUpb_EncodeStatus_Ok||(!data&&n))throw std::runtime_error(std::string("upb encode status: ")+upb_EncodeStatus_String(status));
    out.assign(n?data:"",n);escape(out.data());return n;
  }
  uint64_t decode(std::string_view wire,bool few)override {
    ArenaScope arena(scratch_.data(),scratch_.size(),options_.heap_arena);
    auto* msg=parse(wire,arena.arena,options_.alias_decode);
    const auto result=consume(msg,few);escape(msg);return result;
  }
  void check_ownership() {
    // A bytes field with sufficient trailing space to avoid tiny-buffer alias fallback.
    std::string wire;wire.push_back(char(0x52));wire.push_back(char(100));wire.append(100,'x');
    alignas(std::max_align_t) std::array<std::byte,kScratchBytes> a,b;
    ArenaScope alias_arena(a.data(),a.size(),false),copy_arena(b.data(),b.size(),false);
    auto* alias_msg=parse(wire,alias_arena.arena,true);
    auto* copy_msg=parse(wire,copy_arena.arena,false);
    auto aliased=upb_Message_GetString(alias_msg,fields_[9],upb_StringView_FromDataAndSize(nullptr,0));
    auto copied=upb_Message_GetString(copy_msg,fields_[9],upb_StringView_FromDataAndSize(nullptr,0));
    const auto base=reinterpret_cast<uintptr_t>(wire.data());
    const auto alias_address=reinterpret_cast<uintptr_t>(aliased.data);
    const auto copy_address=reinterpret_cast<uintptr_t>(copied.data);
    if(aliased.size!=100||copied.size!=100||alias_address<base||alias_address>=base+wire.size()||(copy_address>=base&&copy_address<base+wire.size()))throw std::runtime_error("upb copy/alias pointer contract smoke failed");
    wire[2]='y';
    if(aliased.data[0]!='y'||copied.data[0]!='x')throw std::runtime_error("upb copy/alias mutation smoke failed");
  }
};
}
std::unique_ptr<Codec> make_upb_codec(bool fixed,bool alias) {
  return std::make_unique<UpbCodec>(UpbOptions{fixed,alias,false,false});
}
std::unique_ptr<Codec> make_upb_codec_ex(UpbOptions options) {
  return std::make_unique<UpbCodec>(options);
}
void upb_adapter_ownership_smoke(){UpbCodec a({});a.check_ownership();}
void upb_adapter_oom_smoke(){
  alignas(std::max_align_t) std::array<std::byte,512> scratch;
  ArenaScope arena(scratch.data(),scratch.size(),false);
  if(upb_Arena_Malloc(arena.arena,100000)!=nullptr)throw std::runtime_error("bounded arena unexpectedly grew");
  for(bool alias:{false,true}) {
    auto codec=make_upb_codec(false,alias);
    // Unknown length-delimited field 99 always needs unknown-data retention.
    std::string unknown("\x9a\x06\xa0\x8d\x06",5);unknown.append(100000,'x');
    bool failed=false;try{codec->decode(unknown,false);}catch(const std::runtime_error& e){failed=std::string(e.what()).find("status: 2")!=std::string::npos;}
    if(!failed)throw std::runtime_error("oversized unknown field did not report arena OOM");
  }
  // A known bytes payload exceeds the arena when decode must copy it.
  auto copying=make_upb_codec(false,false);
  std::string payload("\x52\xa0\x8d\x06",4);payload.append(100000,'y');
  bool failed=false;try{copying->decode(payload,false);}catch(const std::runtime_error& e){failed=std::string(e.what()).find("status: 2")!=std::string::npos;}
  if(!failed)throw std::runtime_error("oversized known payload did not report arena OOM");
}
}
