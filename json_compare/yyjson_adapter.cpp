#include "adapter.hpp"
#include "vendor/yyjson/yyjson.h"
#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>
namespace jsonbench {
namespace {
class Adapter final:public Codec {
  Shape shape; yyjson_mut_doc* enc=nullptr; yyjson_doc* dec=nullptr;
  std::array<yyjson_mut_val*,9> fields{};
  std::array<yyjson_mut_val*,3> strings{};
  std::array<std::string,3> backing;
  std::vector<char> output;
  std::vector<std::max_align_t> arena; yyjson_alc allocator{};
  static uint64_t u64(yyjson_val*v){if(!yyjson_is_uint(v))throw std::runtime_error("expected unsigned integer");return yyjson_get_uint(v);}
  static uint32_t u32(yyjson_val*v){auto n=u64(v);if(n>UINT32_MAX)throw std::runtime_error("u32 overflow");return uint32_t(n);}
  static void str(yyjson_val*v,std::string&s){if(!yyjson_is_str(v))throw std::runtime_error("expected string");s.assign(yyjson_get_str(v),yyjson_get_len(v));}
  yyjson_mut_val* add(yyjson_mut_val*obj,const char*k,yyjson_mut_val*v){
    if(!v||!yyjson_mut_obj_add(obj,yyjson_mut_str(enc,k),v))throw std::bad_alloc();return v;
  }
public:
  explicit Adapter(Shape s):shape(s){
    enc=yyjson_mut_doc_new(nullptr);if(!enc)throw std::bad_alloc();
    auto*root=yyjson_mut_obj(enc);yyjson_mut_doc_set_root(enc,root);
    auto*common=shape==Shape::Flat?root:add(root,"common",yyjson_mut_obj(enc));
    const char*keys[]={"id","seq","timestamp","value0","value1","producer","stream"};
    for(size_t i=0;i<7;i++)fields[i]=add(common,keys[i],yyjson_mut_uint(enc,0));
    auto*event=shape==Shape::Flat?root:add(root,"http",yyjson_mut_obj(enc));
    fields[7]=add(event,shape==Shape::Flat?"type":"status",yyjson_mut_uint(enc,0));
    fields[8]=add(event,shape==Shape::Flat?"flags":"method",yyjson_mut_uint(enc,0));
    if(shape==Shape::Flat)strings[0]=add(event,"payload",yyjson_mut_str(enc,""));
    else{size_t i=0;for(auto k:{"url","host","cookie"})strings[i++]=add(event,k,yyjson_mut_str(enc,""));}
  }
  ~Adapter()override{if(dec)yyjson_doc_free(dec);if(enc)yyjson_mut_doc_free(enc);}
  const char*name()const override{return "yyjson";}
  void load(const Record&r)override{
    const auto&h=r.h;std::array<uint64_t,9>a={h.id,h.seq,h.timestamp,h.value0,h.value1,h.producer,h.stream,h.type,h.flags};
    for(size_t i=0;i<9;i++)yyjson_mut_set_uint(fields[i],a[i]);
    size_t total=0;for(size_t i=0;i<(shape==Shape::Flat?1:3);i++){backing[i]=r.parts[i];yyjson_mut_set_strn(strings[i],backing[i].data(),backing[i].size());total+=backing[i].size();}
    size_t need=2048+8*total;if(output.size()<need)output.resize(need);
  }
  std::string_view encode()override{
    yyjson_write_err err{};size_t n=yyjson_mut_write_buf(output.data(),output.size(),enc,0,&err);
    if(!n)throw std::runtime_error(std::string("yyjson write: ")+(err.msg?err.msg:"unknown"));return {output.data(),n};
  }
  void parse(std::string_view input)override{
    if(dec){yyjson_doc_free(dec);dec=nullptr;}
    size_t need=yyjson_read_max_memory_usage(input.size(),0);size_t count=(need+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t);
    if(count>arena.size()){
      arena.resize(count);if(!yyjson_alc_pool_init(&allocator,arena.data(),arena.size()*sizeof(std::max_align_t)))throw std::bad_alloc();
    }
    yyjson_read_err err{};dec=yyjson_read_opts(const_cast<char*>(input.data()),input.size(),0,&allocator,&err);
    if(!dec)throw std::runtime_error(std::string("yyjson parse: ")+(err.msg?err.msg:"unknown"));
  }
  void read_back(Record&r)const override{
    auto*root=yyjson_doc_get_root(dec);if(!yyjson_is_obj(root))throw std::runtime_error("expected root object");
    auto*c=shape==Shape::Flat?root:yyjson_obj_get(root,"common");auto*e=shape==Shape::Flat?root:yyjson_obj_get(root,"http");
    if(!yyjson_is_obj(c)||!yyjson_is_obj(e))throw std::runtime_error("expected schema objects");
    r.h={u64(yyjson_obj_get(c,"id")),u64(yyjson_obj_get(c,"seq")),u64(yyjson_obj_get(c,"timestamp")),u64(yyjson_obj_get(c,"value0")),u64(yyjson_obj_get(c,"value1")),u32(yyjson_obj_get(c,"producer")),u32(yyjson_obj_get(c,"stream")),u32(yyjson_obj_get(e,shape==Shape::Flat?"type":"status")),u32(yyjson_obj_get(e,shape==Shape::Flat?"flags":"method"))};
    if(shape==Shape::Flat){str(yyjson_obj_get(e,"payload"),r.parts[0]);r.parts[1].clear();r.parts[2].clear();}
    else{size_t i=0;for(auto k:{"url","host","cookie"})str(yyjson_obj_get(e,k),r.parts[i++]);}
  }
  const void*parsed_state()const override{return dec;}
};
}
std::unique_ptr<Codec>make_yyjson(Shape s){return std::make_unique<Adapter>(s);}
}
