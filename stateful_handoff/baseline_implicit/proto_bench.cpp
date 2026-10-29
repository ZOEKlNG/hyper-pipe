#include "encoding.hpp"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <time.h>
#include <sched.h>
#ifdef ALLOC_AUDIT
thread_local bool counting=false;
thread_local uint64_t alloc_calls=0,free_calls=0,alloc_bytes=0;
void*operator new(size_t n){if(counting){alloc_calls++;alloc_bytes+=n;}if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void*operator new[](size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{if(counting&&p)free_calls++;std::free(p);}
void operator delete[](void*p)noexcept{::operator delete(p);}
void operator delete(void*p,size_t)noexcept{::operator delete(p);}
void operator delete[](void*p,size_t)noexcept{::operator delete(p);}
#endif
using namespace sf;
uint64_t clock_ns(clockid_t c=CLOCK_MONOTONIC){timespec t;clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
statefulpb::Module*one_module(statefulpb::OneofEvent&e,unsigned m){switch(m){case 1:return e.mutable_tls();case 2:return e.mutable_http();default:return e.mutable_dns();}}
struct PBCase {
 unsigned scheme,pattern;size_t length;statefulpb::OneofEvent one;statefulpb::ParallelEvent parallel;
 std::array<std::string,4>text;std::array<uint8_t,16384>wire;
 PBCase(unsigned s,unsigned p,size_t n):scheme(s),pattern(p),length(n){for(size_t i=0;i<4;i++)text[i]=std::string(n,char('a'+i));for(unsigned m=1;m<4;m++){fill(one_module(one,m),1);fill(mutable_module(parallel,m),1);}parallel.Clear();}
 void fill(statefulpb::Module*m,uint64_t i){m->set_value0(i+1);m->set_value1(i*73+9);for(size_t f=0;f<4;f++)set_text(*m,f,{text[f].data(),text[f].size()});}
 size_t run(uint64_t i){
  unsigned mod=pattern==0?1:pattern==1?1+i%3:pattern==2?1+(i/64)%3:1;
  if(scheme<2){if(scheme==0)one.Clear();one.set_event_id(i+1);auto*c=one.mutable_common();c->set_value0(i+5);c->set_value1(i*9+7);auto*m=one_module(one,mod);if(scheme==1)m->Clear();fill(m,i);size_t n=one.ByteSizeLong();one.SerializeWithCachedSizesToArray(wire.data());escape(wire.data());return n;}
  if(scheme==2)parallel.Clear();else{parallel.clear_common();parallel.clear_tls();parallel.clear_http();parallel.clear_dns();}
  parallel.set_event_id(i+1);auto*c=parallel.mutable_common();c->set_value0(i+5);c->set_value1(i*9+7);fill(mutable_module(parallel,mod),i);if(pattern==3){fill(parallel.mutable_tls(),i);fill(parallel.mutable_http(),i);}
  size_t n=parallel.ByteSizeLong();parallel.SerializeWithCachedSizesToArray(wire.data());escape(wire.data());return n;
 }
};
const char*scheme_name(unsigned s){return std::array<const char*,4>{"P1_oneof_parent_clear","P1_oneof_child_clear","P2_parallel_parent_clear","P2_parallel_children_clear"}[s];}
const char*pattern_name(unsigned p){return std::array<const char*,4>{"fixed_tls","alternate_tls_http_dns","blocks64","mixed_tls_http_P2_only"}[p];}
int main(int argc,char**argv){try{
 cpu_set_t affinity;CPU_ZERO(&affinity);CPU_SET(0,&affinity);require(!sched_setaffinity(0,sizeof(affinity),&affinity),"CPU0 pin failed");
 std::cout<<std::setprecision(12);
 if(argc>1&&std::string(argv[1])=="--check"){
  size_t checked=0;for(size_t n:{12,64,512})for(unsigned p=0;p<3;p++)for(uint64_t i=0;i<257;i++){
   PBCase a(0,p,n),b(2,p,n);auto na=a.run(i),nb=b.run(i);require(na==nb&&!std::memcmp(a.wire.data(),b.wire.data(),na),"single-detail wire differs");checked++;
  }
  statefulpb::OneofEvent one;one.mutable_tls()->set_value0(1);one.mutable_http()->set_value0(2);statefulpb::ParallelEvent two;two.mutable_tls()->set_value0(1);two.mutable_http()->set_value0(2);
  require(!one.has_tls()&&one.has_http()&&two.has_tls()&&two.has_http(),"oneof coexistence test");std::cout<<"PASS single_detail_wire_equivalence="<<checked<<" mixed_oneof_discards_tls=true parallel_preserves_both=true\n";return 0;
 }
 std::cout<<"scheme,pattern,length,repeat,iterations,wall_ns,cpu_ns,cpu_ns_per_event,checksum,alloc_calls,free_calls,alloc_bytes\n";
 for(unsigned scheme=0;scheme<4;scheme++)for(unsigned pattern=0;pattern<4;pattern++){
  if(pattern==3&&scheme<2)continue;
  for(size_t n:{12,64,512})for(int rep=0;rep<5;rep++){
   PBCase c(scheme,pattern,n);uint64_t count=4096,sum=0;
   for(size_t i=0;i<4096;i++)sum+=c.run(i);
#ifdef ALLOC_AUDIT
   count=10000;alloc_calls=free_calls=alloc_bytes=0;counting=true;
#endif
   for(;;){auto cpu0=clock_ns(CLOCK_THREAD_CPUTIME_ID),wall0=clock_ns();sum=0;for(uint64_t i=0;i<count;i++)sum+=c.run(i);auto wall=clock_ns()-wall0,cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;
#ifdef ALLOC_AUDIT
    counting=false;auto ac=alloc_calls,fc=free_calls,ab=alloc_bytes;
#else
    uint64_t ac=0,fc=0,ab=0;if(wall<80000000){count=uint64_t(double(count)*88000000/std::max(wall,uint64_t(1))/4096+1)*4096;continue;}
#endif
    std::cout<<scheme_name(scheme)<<','<<pattern_name(pattern)<<','<<n<<','<<rep<<','<<count<<','<<wall<<','<<cpu<<','<<double(cpu)/count<<','<<sum<<','<<ac<<','<<fc<<','<<ab<<'\n';std::cout.flush();break;
   }
  }
 }return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
