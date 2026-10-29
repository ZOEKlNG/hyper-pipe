#pragma once
#include "storage.hpp"
#include <algorithm>
#include <new>
namespace sf {
struct Descriptor {uint16_t module,offset;uint32_t reserved;uint64_t mask;};
struct PointerDescriptor {uint16_t module;uint16_t reserved0;uint32_t reserved1;uint64_t mask;const Export*ptr;};
static_assert(sizeof(Descriptor)==16&&sizeof(PointerDescriptor)==24);
struct alignas(64) Header {
 uint64_t sequence=0,id=0,mask=0;unsigned phase=0,used=0,temp_used=0,count=0;
 Session*retiring=nullptr;Session*session=nullptr;
 std::shared_ptr<Session>hold;
 std::array<char,32>temp{};
};
struct AllFields{std::array<Export,MAX_MODULES>mod;};
struct Layout {
 size_t stride,body_size;Handoff mode;
 Layout(Handoff h,size_t len,size_t mods):mode(h){
  size_t text_fields=0;for(size_t m=0;m<mods;m++)text_fields+=string_count(m);
  body_size=h==Handoff::H1?sizeof(AllFields)+text_fields*len:
   h==Handoff::H2?MAX_MODULES*(2*9+STRINGS*13)+64:
   h==Handoff::H3||h==Handoff::H4?sizeof(AllFields):
   h==Handoff::H5?MAX_MODULES*sizeof(Descriptor)+MAX_MODULES*sizeof(Export):MAX_MODULES*sizeof(PointerDescriptor);
  stride=(sizeof(Header)+body_size+63)&~size_t(63);
 }
};
struct RingMemory {
 Layout layout;size_t capacity;std::unique_ptr<std::byte[],void(*)(std::byte*)>memory;
 RingMemory(Handoff h,size_t len,size_t mods,size_t cap):layout(h,len,mods),capacity(cap),memory(static_cast<std::byte*>(::operator new[](layout.stride*cap,std::align_val_t(64))),[](std::byte*p){::operator delete[](p,std::align_val_t(64));}){
  for(size_t i=0;i<capacity;i++)new(memory.get()+i*layout.stride)Header;
 }
 ~RingMemory(){for(size_t i=0;i<capacity;i++)header(i).~Header();}
 Header&header(uint64_t seq){return *std::launder(reinterpret_cast<Header*>(memory.get()+(seq&(capacity-1))*layout.stride));}
 std::byte*body(Header&h){return reinterpret_cast<std::byte*>(&h)+sizeof(Header);}
};
inline void store_bytes(std::byte*&p,const void*src,size_t n){std::memcpy(p,src,n);p+=n;}
inline void build(Header&h,std::byte*body,Session&s,unsigned phase,const Config&c,const std::array<char,32>&tmp,bool make_version=true){
 h.id=s.id;h.phase=phase;h.mask=s.extracted_bits&rule_mask(c.modules,phase,c.sparse);h.count=0;h.used=0;h.temp_used=0;h.session=&s;
 if((h.mask&TEMP_BIT)&&c.handoff!=Handoff::H2){std::memcpy(h.temp.data(),tmp.data(),tmp.size());h.temp_used=tmp.size();}
 if(c.handoff==Handoff::H1||c.handoff==Handoff::H3){
  size_t data_used=0;
  for(uint64_t bits=h.mask&~TEMP_BIT;bits;bits&=bits-1){unsigned tag=std::countr_zero(bits);size_t m=tag/6,f=tag%6;
   size_t off=m*sizeof(Export)+(f<2?offsetof(Export,value)+f*8:offsetof(Export,text)+(f-2)*sizeof(View));
   if(f<2)std::memcpy(body+off,&s.field(m).value[f],8);
   else{auto v=s.field(m).text[f-2];if(c.handoff==Handoff::H1){auto*p=reinterpret_cast<char*>(body+sizeof(AllFields)+data_used);std::memcpy(p,v.data,v.size);v={p,v.size};data_used+=v.size;}std::memcpy(body+off,&v,sizeof(v));}
  }h.used=data_used;
 }else if(c.handoff==Handoff::H4){
  // Mirror the complete, fixed maximum layout, including inactive modules.
  std::memcpy(body,s.bulk.get(),sizeof(AllFields));h.used=sizeof(AllFields);
 }else if(c.handoff==Handoff::H2){
  auto*p=body;
  for(uint64_t bits=h.mask&~TEMP_BIT;bits;bits&=bits-1){uint8_t tag=std::countr_zero(bits);size_t m=tag/6,f=tag%6;store_bytes(p,&tag,1);
   if(f<2){uint64_t v=s.field(m).value[f];store_bytes(p,&v,8);}else{auto v=s.field(m).text[f-2];uint32_t n=v.size;uintptr_t ptr=reinterpret_cast<uintptr_t>(v.data);store_bytes(p,&n,4);store_bytes(p,&ptr,8);}
  }
  if(h.mask&TEMP_BIT){uint8_t tag=63;uint32_t n=tmp.size()|(uint32_t(1)<<31);store_bytes(p,&tag,1);store_bytes(p,&n,4);store_bytes(p,tmp.data(),tmp.size());}h.used=p-body;
 }else if(c.handoff==Handoff::H5){
  for(size_t m=0;m<c.modules;m++){uint64_t mask=(h.mask>>(m*6))&63;if(!mask)continue;Descriptor d{uint16_t(m),uint16_t(h.count*sizeof(Export)),0,mask};std::memcpy(body+h.count*sizeof(Descriptor),&d,sizeof(d));const Export&v=s.field(m);std::memcpy(body+MAX_MODULES*sizeof(Descriptor)+d.offset,&v,sizeof(v));h.count++;}h.used=h.count*(sizeof(Descriptor)+sizeof(Export));
 }else{
  // Live counters cannot safely be borrowed. Versions are written once per
  // emission, retained in the session until all four records are consumed.
  for(size_t m=0;m<c.modules;m++){uint64_t mask=(h.mask>>(m*6))&63;if(!mask)continue;
   auto&v=s.frozen[phase*c.modules+m];if(make_version)v=s.field(m);PointerDescriptor d{uint16_t(m),0,0,mask,&v};std::memcpy(body+h.count*sizeof(d),&d,sizeof(d));h.count++;}h.used=h.count*sizeof(PointerDescriptor);
 }
 escape(body);
}
struct Decoded {std::array<Export,MAX_MODULES>modules{};View temp;};
inline void decode(const Header&h,const std::byte*body,Handoff mode,Decoded&d){
 // Only selected fields are read; no whole-body aliasing/reinterpret of raw data.
 if(mode==Handoff::H1||mode==Handoff::H3||mode==Handoff::H4){
  for(uint64_t bits=h.mask&~TEMP_BIT;bits;bits&=bits-1){unsigned tag=std::countr_zero(bits);size_t m=tag/6,f=tag%6;size_t off=m*sizeof(Export)+(f<2?offsetof(Export,value)+f*8:offsetof(Export,text)+(f-2)*sizeof(View));void*dst=f<2?static_cast<void*>(&d.modules[m].value[f]):static_cast<void*>(&d.modules[m].text[f-2]);std::memcpy(dst,body+off,f<2?8:sizeof(View));}
 }else if(mode==Handoff::H2){
  auto*p=body;auto*end=body+h.used;
  while(p<end){uint8_t tag;std::memcpy(&tag,p++,1);if(tag==63){uint32_t n;std::memcpy(&n,p,4);p+=4;n&=0x7fffffff;d.temp={reinterpret_cast<const char*>(p),n};p+=n;continue;}
   size_t m=tag/6,f=tag%6;if(f<2){std::memcpy(&d.modules[m].value[f],p,8);p+=8;}else{uint32_t n;uintptr_t ptr;std::memcpy(&n,p,4);p+=4;std::memcpy(&ptr,p,8);p+=8;d.modules[m].text[f-2]={reinterpret_cast<const char*>(ptr),n};}
  }require(p==end,"tape boundary");
 }else for(size_t i=0;i<h.count;i++){
  if(mode==Handoff::H5){Descriptor entry;std::memcpy(&entry,body+i*sizeof(entry),sizeof(entry));std::memcpy(&d.modules[entry.module],body+MAX_MODULES*sizeof(Descriptor)+entry.offset,sizeof(Export));}
  else{PointerDescriptor entry;std::memcpy(&entry,body+i*sizeof(entry),sizeof(entry));d.modules[entry.module]=*entry.ptr;}
 }
 if((h.mask&TEMP_BIT)&&mode!=Handoff::H2)d.temp={h.temp.data(),h.temp_used};
}
} // namespace sf
