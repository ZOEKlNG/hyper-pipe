#include "adapter.hpp"
#include <array>
#include <iostream>
#include <limits>

using namespace dynbench;
Plan plan(uint16_t mask) {
  const size_t offsets[]={0,8,16,24,32,40,44,48,52,0};
  Plan p;p.mask=mask;p.name="smoke";
  for(uint32_t fid=1;fid<=10;fid++)if(p.has(fid)) {
    FieldOp op;op.fid=fid;op.kind=fid<=5?ScalarKind::U64:fid<=9?ScalarKind::U32:ScalarKind::Bytes;op.is_payload=fid==10;op.source_offset=offsets[fid-1];p.ops.push_back(op);
  }
  return p;
}
void check(bool condition,const char* error){if(!condition)throw std::runtime_error(error);}
int main() {
  Event e;e.h={UINT64_MAX,2,1770000000000000000ULL,UINT64_MAX-5,0,UINT32_MAX,3,7,0,64,1,0};
  size_t cases=0;
  for(bool fixed:{false,true})for(bool alias:{false,true})for(bool heap:{false,true})for(bool copy_input:{false,true}) {
    auto c=make_upb_codec_ex({fixed,alias,copy_input,heap});std::string wire;
    for(size_t payload_n:{0u,1u,15u,16u,64u,256u,1024u}) {
      e.payload.resize(payload_n);for(size_t i=0;i<payload_n;i++)e.payload[i]=static_cast<char>(i*71);
      // Repeated full->empty->sparse transitions ensure no stale field/payload survives.
      for(uint16_t mask:std::array<uint16_t,7>{1023,0,3,128,512,341,1023}) {
        const auto p=plan(mask);size_t n=c->encode(e,p,wire);check(n==wire.size(),"length");
        for(bool few:{false,true})check(c->decode(wire,few)==expected_checksum(e,p,few),"roundtrip logical checksum");
        auto copied_wire=wire;c->decode(wire,false);check(copied_wire==wire,"decode mutated external wire");cases++;
      }
    }
    for(std::string bad:{std::string(1,'\0'),std::string("\x52\x80",2),std::string("\x08\x80",2)}) {
      bool rejected=false;try{c->decode(bad,false);}catch(const std::exception&){rejected=true;}
      check(rejected,"malformed wire accepted");
    }
  }
  upb_adapter_ownership_smoke();
  upb_adapter_oom_smoke();
  auto bounded=make_upb_codec(false,false);e.payload.assign(8193,'x');std::string out;
  bool rejected=false;try{bounded->encode(e,plan(1023),out);}catch(const std::exception&){rejected=true;}
  check(rejected,"configured payload max not enforced");
  std::cout<<"PASS official upb v29.3 runtime MiniTable cases="<<cases<<" full/sparse/default/maxint/binary payload; malformed rejected; alias/copy ownership proved; input wire unchanged; configured size bound enforced\n";
}
