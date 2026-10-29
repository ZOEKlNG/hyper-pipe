#include "adapter.hpp"
#include "../core_handoff/common.hpp"
#include <stdexcept>
namespace jsonbench {
namespace {
class Adapter final:public Codec {
  Shape shape;handoff::PBState enc,dec;std::array<uint8_t,8192>output{};
  handoff::Shape pbshape()const{return shape==Shape::Flat?handoff::Shape::Flat:handoff::Shape::Nested;}
public:
  explicit Adapter(Shape s):shape(s){}
  const char*name()const override{return "protobuf_fixed";}
  void load(const Record&r)override{
    const auto&h=r.h;handoff::SourceView v;
    v.h={h.id,h.seq,h.timestamp,h.value0,h.value1,h.producer,h.stream,h.type,h.flags,64,1,0};
    for(size_t i=0;i<3;i++)v.parts[i]={r.parts[i].data(),uint32_t(r.parts[i].size())};
    v.flat={r.parts[0].data(),uint32_t(r.parts[0].size())};handoff::fill_pb(enc,pbshape(),v);
  }
  std::string_view encode()override{size_t n=handoff::serialize_pb(enc,pbshape(),output.data(),output.size());return {reinterpret_cast<const char*>(output.data()),n};}
  void parse(std::string_view s)override{
    bool ok=shape==Shape::Flat?dec.flat.ParseFromArray(s.data(),int(s.size())):dec.nested.ParseFromArray(s.data(),int(s.size()));
    if(!ok)throw std::runtime_error("invalid protobuf");
  }
  void read_back(Record&r)const override{
    if(shape==Shape::Flat){const auto&m=dec.flat;r.h={m.id(),m.seq(),m.timestamp(),m.value0(),m.value1(),m.producer(),m.stream(),m.type(),m.flags()};r.parts[0]=m.payload();r.parts[1].clear();r.parts[2].clear();}
    else{const auto&c=dec.nested.common();const auto&m=dec.nested.http();r.h={c.id(),c.seq(),c.timestamp(),c.value0(),c.value1(),c.producer(),c.stream(),m.status(),m.method()};r.parts[0]=m.url();r.parts[1]=m.host();r.parts[2]=m.cookie();}
  }
  const void*parsed_state()const override{return shape==Shape::Flat?static_cast<const void*>(&dec.flat):static_cast<const void*>(&dec.nested);}
};
}
std::unique_ptr<Codec>make_protobuf(Shape s){return std::make_unique<Adapter>(s);}
}
