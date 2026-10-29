#include "adapter.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <stdexcept>
namespace jsonbench {
namespace {
using J=nlohmann::json;
constexpr const char* wide_keys[]={"id","seq","timestamp","value0","value1"};
constexpr const char* narrow_keys[]={"producer","stream","type","flags"};
class Adapter final:public Codec {
  Shape shape; J enc,dec; std::string output;
  std::array<J*,9> fields{}; std::array<std::string*,3> strings{};
  static uint64_t u64(const J& j){
    if(!j.is_number_unsigned())throw std::runtime_error("expected unsigned integer");
    return j.get<uint64_t>();
  }
  static uint32_t u32(const J& j){auto n=u64(j);if(n>UINT32_MAX)throw std::runtime_error("u32 overflow");return uint32_t(n);}
public:
  explicit Adapter(Shape s):shape(s){
    J* common=shape==Shape::Flat?&enc:&enc["common"];
    for(size_t i=0;i<5;i++)fields[i]=&((*common)[wide_keys[i]]=uint64_t(0));
    for(size_t i=0;i<2;i++)fields[5+i]=&((*common)[narrow_keys[i]]=uint32_t(0));
    J* event=shape==Shape::Flat?&enc:&enc["http"];
    fields[7]=&((*event)[shape==Shape::Flat?"type":"status"]=uint32_t(0));
    fields[8]=&((*event)[shape==Shape::Flat?"flags":"method"]=uint32_t(0));
    if(shape==Shape::Flat){(*event)["payload"]=std::string();strings[0]=&(*event)["payload"].get_ref<std::string&>();}
    else{size_t i=0;for(auto k:{"url","host","cookie"}){(*event)[k]=std::string();strings[i++]=&(*event)[k].get_ref<std::string&>();}}
  }
  const char*name()const override{return "nlohmann_json";}
  void load(const Record&r)override{
    const auto&h=r.h;
    std::array<uint64_t,5>a={h.id,h.seq,h.timestamp,h.value0,h.value1};
    std::array<uint32_t,4>b={h.producer,h.stream,h.type,h.flags};
    for(size_t i=0;i<5;i++)*fields[i]=a[i];for(size_t i=0;i<4;i++)*fields[5+i]=b[i];
    for(size_t i=0;i<(shape==Shape::Flat?1:3);i++)*strings[i]=r.parts[i];
  }
  std::string_view encode()override{output=enc.dump(-1,' ',false,J::error_handler_t::strict);return output;}
  void parse(std::string_view s)override{dec=J::parse(s.begin(),s.end(),nullptr,true,false);}
  void read_back(Record&r)const override{
    const auto&c=shape==Shape::Flat?dec:dec.at("common");const auto&e=shape==Shape::Flat?dec:dec.at("http");
    r.h={u64(c.at("id")),u64(c.at("seq")),u64(c.at("timestamp")),u64(c.at("value0")),u64(c.at("value1")),u32(c.at("producer")),u32(c.at("stream")),u32(e.at(shape==Shape::Flat?"type":"status")),u32(e.at(shape==Shape::Flat?"flags":"method"))};
    if(shape==Shape::Flat){r.parts[0]=e.at("payload").get_ref<const std::string&>();r.parts[1].clear();r.parts[2].clear();}
    else{size_t i=0;for(auto k:{"url","host","cookie"})r.parts[i++]=e.at(k).get_ref<const std::string&>();}
  }
  const void*parsed_state()const override{return &dec;}
};
}
std::unique_ptr<Codec>make_nlohmann(Shape s){return std::make_unique<Adapter>(s);}
}
