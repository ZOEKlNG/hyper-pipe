#pragma once
#include "slots.hpp"
#include "events.pb.h"
namespace sf {
inline statefulpb::Module*mutable_module(statefulpb::ParallelEvent&e,size_t m){
 switch(m){case 0:return e.mutable_common();case 1:return e.mutable_tls();case 2:return e.mutable_http();case 3:return e.mutable_dns();case 4:return e.mutable_waf();case 5:return e.mutable_risk();case 6:return e.mutable_extra6();default:return e.mutable_extra7();}
}
inline const statefulpb::Module&get_module(const statefulpb::ParallelEvent&e,size_t m){
 switch(m){case 0:return e.common();case 1:return e.tls();case 2:return e.http();case 3:return e.dns();case 4:return e.waf();case 5:return e.risk();case 6:return e.extra6();default:return e.extra7();}
}
inline bool has_module(const statefulpb::ParallelEvent&e,size_t m){switch(m){case 0:return e.has_common();case 1:return e.has_tls();case 2:return e.has_http();case 3:return e.has_dns();case 4:return e.has_waf();case 5:return e.has_risk();case 6:return e.has_extra6();default:return e.has_extra7();}}
inline void set_text(statefulpb::Module&m,size_t f,View v){switch(f){case 0:m.set_text0(v.data,v.size);break;case 1:m.set_text1(v.data,v.size);break;case 2:m.set_text2(v.data,v.size);break;default:m.set_text3(v.data,v.size);}}
inline const std::string&get_text(const statefulpb::Module&m,size_t f){switch(f){case 0:return m.text0();case 1:return m.text1();case 2:return m.text2();default:return m.text3();}}
struct Encoder {
 statefulpb::ParallelEvent event;Decoded decoded;
 std::array<uint8_t,32768>wire{};
 Encoder(){for(size_t m=0;m<MAX_MODULES;m++)mutable_module(event,m);}
 size_t encode(const Header&h,const std::byte*body,Handoff mode){
  decode(h,body,mode,decoded);event.Clear();event.set_event_id(h.id*4+h.phase);
  for(size_t m=0;m<MAX_MODULES;m++){
   uint64_t mask=(h.mask>>(m*6))&63;if(!mask)continue;auto*p=mutable_module(event,m);const auto&v=decoded.modules[m];
   if(mask&1)p->set_value0(v.value[0]);if(mask&2)p->set_value1(v.value[1]);for(size_t f=0;f<STRINGS;f++)if(mask&(uint64_t(1)<<(f+2)))set_text(*p,f,v.text[f]);
  }
  if(h.mask&TEMP_BIT)event.set_xff(decoded.temp.data,decoded.temp.size);
  size_t n=event.ByteSizeLong();require(n<=wire.size(),"wire capacity");auto*end=event.SerializeWithCachedSizesToArray(wire.data());require(end==wire.data()+n,"encoded length");escape(wire.data());return n;
 }
 bool verify(const Header&h,size_t n,const Corpus&corpus){
  statefulpb::ParallelEvent p;if(!p.ParseFromArray(wire.data(),n)||p.event_id()!=h.id*4+h.phase)return false;
  for(size_t m=0;m<MAX_MODULES;m++){
   uint64_t mask=(h.mask>>(m*6))&63;if(has_module(p,m)!=(mask!=0))return false;if(!mask)continue;auto&v=get_module(p,m);
   if(v.value0()!=((mask&1)?scalar_value(h.id,h.phase,m,0):0)||v.value1()!=((mask&2)?scalar_value(h.id,h.phase,m,1):0))return false;
   for(size_t f=0;f<STRINGS;f++){std::string_view expected=(mask&(uint64_t(1)<<(f+2)))?corpus.get(h.id,m,f):std::string_view();if(get_text(v,f)!=expected)return false;}
  }
  auto tmp=temp_bytes(h.id,h.phase);std::string_view expected=(h.mask&TEMP_BIT)?std::string_view(tmp.data(),tmp.size()):std::string_view();return p.xff()==expected;
 }
};
} // namespace sf
