#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <stdexcept>
#include <bit>
namespace dynbench {
struct Header {uint64_t id,seq,timestamp,value0,value1;uint32_t producer,stream,type,flags;uint16_t header_len,version;uint32_t payload_len;};
static_assert(sizeof(Header)==64 && offsetof(Header,payload_len)==60);
struct Event {Header h{};std::string payload;};
enum class ScalarKind:uint8_t {U32,U64,Fixed32,Fixed64,Bytes};
struct FieldOp {
 uint32_t fid=0;ScalarKind kind=ScalarKind::U64;size_t source_offset=0;bool is_payload=false;
 std::string tag_bytes;       // Protobuf varint wire tag (or bytes tag for payload)
 std::string fixed_tag_bytes; // Protobuf fixed32/fixed64 wire tag (bytes tag for payload)
};
struct Plan {
 std::string name;std::vector<FieldOp> ops;uint16_t mask=0;
 bool has(uint32_t fid)const{return fid>=1&&fid<=10&&(mask&(uint16_t(1)<<(fid-1)));}
};
class Codec {
public:
 virtual const char* name()const=0;
 virtual size_t encode(const Event&,const Plan&,std::string&)=0;
 // few=true consumes only id+seq+type; false consumes all logical fields and payload view.
 // Absent fields default to zero/empty. A present payload contributes len+first+last, even if zero length.
 virtual uint64_t decode(std::string_view,bool few)=0;
 virtual ~Codec()=default;
};
inline uint64_t scalar_value(const Event&e,const FieldOp&op){
 if(op.is_payload)return 0;
 if(op.kind==ScalarKind::U32||op.kind==ScalarKind::Fixed32){uint32_t v;std::memcpy(&v,reinterpret_cast<const char*>(&e.h)+op.source_offset,4);return v;}
 uint64_t v;std::memcpy(&v,reinterpret_cast<const char*>(&e.h)+op.source_offset,8);return v;
}
inline uint64_t payload_checksum(std::string_view p){return p.size()+(p.empty()?0:uint8_t(p.front())+uint8_t(p.back()));}
inline uint64_t expected_checksum(const Event&e,const Plan&p,bool few){uint64_t s=0;for(const auto&op:p.ops){if(few&&op.fid!=1&&op.fid!=2&&op.fid!=8)continue;s+=op.is_payload?payload_checksum(e.payload):scalar_value(e,op);}return s;}
inline void escape(const void*p){asm volatile("" : : "g"(p) : "memory");}
} // namespace dynbench
