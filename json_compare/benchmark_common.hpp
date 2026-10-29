#pragma once
#include "adapter.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <vector>
namespace jsonbench {
using namespace std::string_view_literals;
constexpr size_t CORPUS_N=4096;
using Factory=std::unique_ptr<Codec>(*)(Shape);
inline Factory factories[]={make_protobuf,make_nlohmann,make_yyjson,make_glaze};
inline const char*shape_name(Shape s){return s==Shape::Flat?"flat":"nested_http";}
inline void escape(const void*p){asm volatile("" : : "g"(p):"memory");}
inline uint64_t witness(std::string_view s){return s.size()+(s.empty()?0:static_cast<unsigned char>(s.front())+static_cast<unsigned char>(s.back()));}
inline uint64_t witness(const Record&r){auto&h=r.h;return h.id+h.seq+h.timestamp+h.value0+h.value1+h.producer+h.stream+h.type+h.flags+witness(r.parts[0])+witness(r.parts[1])+witness(r.parts[2]);}
inline void base64_encode(std::string_view in,std::string&out){
  constexpr char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.resize((in.size()+2)/3*4);size_t i=0,j=0;
  while(i+3<=in.size()){uint32_t v=(uint32_t(uint8_t(in[i]))<<16)|(uint32_t(uint8_t(in[i+1]))<<8)|uint8_t(in[i+2]);i+=3;out[j++]=t[v>>18];out[j++]=t[(v>>12)&63];out[j++]=t[(v>>6)&63];out[j++]=t[v&63];}
  if(i<in.size()){uint32_t v=uint32_t(uint8_t(in[i]))<<16;bool two=i+1<in.size();if(two)v|=uint32_t(uint8_t(in[i+1]))<<8;out[j++]=t[v>>18];out[j++]=t[(v>>12)&63];out[j++]=two?t[(v>>6)&63]:'=';out[j++]='=';}
}
inline void base64_decode(std::string_view in,std::string&out){
  static const auto table=[](){std::array<int16_t,256>a{};a.fill(-1);std::string_view t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";for(size_t i=0;i<t.size();i++)a[uint8_t(t[i])]=int16_t(i);return a;}();
  if(in.size()%4)throw std::runtime_error("bad base64 length");out.clear();out.reserve(in.size()/4*3);
  for(size_t i=0;i<in.size();i+=4){int a=table[uint8_t(in[i])],b=table[uint8_t(in[i+1])];bool p2=in[i+2]=='=',p3=in[i+3]=='=';int c=p2?0:table[uint8_t(in[i+2])],d=p3?0:table[uint8_t(in[i+3])];
    if(a<0||b<0||c<0||d<0||(p2&&!p3)||((p2||p3)&&i+4!=in.size())||(p2&&(b&15))||(p3&&!p2&&(c&3)))throw std::runtime_error("bad canonical base64");
    uint32_t v=(uint32_t(a)<<18)|(uint32_t(b)<<12)|(uint32_t(c)<<6)|uint32_t(d);out.push_back(char(v>>16));if(!p2)out.push_back(char(v>>8));if(!p3)out.push_back(char(v));
  }
}
inline void transform(const Record&in,Record&out,Shape shape,bool encode){out.h=in.h;for(size_t i=0;i<(shape==Shape::Flat?1:3);i++){if(encode)base64_encode(in.parts[i],out.parts[i]);else base64_decode(in.parts[i],out.parts[i]);}if(shape==Shape::Flat){out.parts[1].clear();out.parts[2].clear();}}
inline std::vector<Record>make_records(Shape shape,size_t tail,bool binary){
  std::mt19937_64 rng(0x4b57a3+tail+uint64_t(binary)*907);std::vector<Record>records;records.reserve(CORPUS_N);
  const std::array<std::string_view,10>tokens={"abcdef","127.0.0.1","?a=b&x=y","\"","\\","\n","\t","\0"sv,"日志","/api/v1/"};
  for(size_t i=0;i<CORPUS_N;i++){
    Record r;r.h={rng(),uint64_t(i+1),1770000000000000000ULL+i*17,rng(),rng(),uint32_t(1+i%32),uint32_t(1+i%64),uint32_t(200+i%7),uint32_t(i%16)};
    std::array<size_t,3>lengths=shape==Shape::Flat?std::array<size_t,3>{tail,0,0}:std::array<size_t,3>{tail/2,tail/4,tail-tail/2-tail/4};
    for(size_t part=0;part<3;part++){
      auto&s=r.parts[part];size_t n=lengths[part];s.reserve(n);
      while(s.size()<n){if(binary)s.push_back(char(rng()));else{auto t=tokens[rng()%tokens.size()];if(t.size()>n-s.size())s.push_back(char('a'+rng()%26));else s.append(t);}}
    }records.push_back(std::move(r));
  }return records;
}
inline nlohmann::ordered_json json_record(const Record&r,Shape shape){
  using J=nlohmann::ordered_json;const auto&h=r.h;
  J c={{"id",h.id},{"seq",h.seq},{"timestamp",h.timestamp},{"value0",h.value0},{"value1",h.value1},{"producer",h.producer},{"stream",h.stream}};
  if(shape==Shape::Flat){c["type"]=h.type;c["flags"]=h.flags;c["payload"]=r.parts[0];return c;}
  return J{{"common",std::move(c)},{"http",J{{"status",h.type},{"method",h.flags},{"url",r.parts[0]},{"host",r.parts[1]},{"cookie",r.parts[2]}}}};
}
} // namespace jsonbench
