#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <stdexcept>
namespace hp {
constexpr size_t HEADER=72, MAX_DATAGRAM=1472;
constexpr uint32_t MAGIC=0x31475048; // LE bytes "HPG1"
inline void u16(std::byte*p,uint16_t x){p[0]=std::byte(x);p[1]=std::byte(x>>8);}
inline void u32(std::byte*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=std::byte(x>>(8*i));}
inline void u64(std::byte*p,uint64_t x){for(unsigned i=0;i<8;i++)p[i]=std::byte(x>>(8*i));}
inline uint16_t r16(const std::byte*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t r32(const std::byte*p){uint32_t x=0;for(unsigned i=0;i<4;i++)x|=uint32_t(p[i])<<(8*i);return x;}
inline uint64_t r64(const std::byte*p){uint64_t x=0;for(unsigned i=0;i<8;i++)x|=uint64_t(p[i])<<(8*i);return x;}
inline uint64_t mix64(uint64_t x){x^=x>>30;x*=0xbf58476d1ce4e5b9ULL;x^=x>>27;x*=0x94d049bb133111ebULL;return x^(x>>31);}
struct Event{uint64_t producer=1,session=1,sequence=0,id=0,timestamp_ns=0;uint32_t partition=0,epoch=1,type=1;};
inline size_t encode(std::byte*p,size_t cap,const Event&e,size_t payload){
 if(payload>MAX_DATAGRAM-HEADER||cap<HEADER+payload)throw std::runtime_error("payload exceeds bounded datagram");
 u32(p,MAGIC);u16(p+4,1);u16(p+6,HEADER);u32(p+8,HEADER+payload);u32(p+12,1);
 u64(p+16,e.producer);u64(p+24,e.session);u64(p+32,e.sequence);u64(p+40,e.id);u64(p+48,e.timestamp_ns);
 u32(p+56,e.partition);u32(p+60,e.epoch);u32(p+64,e.type);u32(p+68,payload);
 for(size_t i=0;i<payload;i++){p[HEADER+i]=std::byte((e.id+i)&255);}
 return HEADER+payload;
}
inline bool valid(const std::byte*p,size_t n){
 if(n<HEADER||n>MAX_DATAGRAM)return false;
 if(r32(p)!=MAGIC||r16(p+4)!=1||r32(p+12)!=1)return false;
 size_t h=r16(p+6);if(h<HEADER||h>n||r32(p+8)!=n)return false;
 return r32(p+68)==n-h;
}
}
