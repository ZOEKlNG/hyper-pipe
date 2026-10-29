// Corrected two-detail supplement: each detail is filled exactly once.
// The original generic fixture refreshed TLS twice; retain its raw data as
// provenance but use this supplement for mixed-message performance conclusions.
#define main original_proto_main
#include "../proto_bench.cpp"
#undef main
int main(){try{
 cpu_set_t affinity;CPU_ZERO(&affinity);CPU_SET(0,&affinity);require(!sched_setaffinity(0,sizeof(affinity),&affinity),"mixed proto CPU0");
 std::cout<<std::setprecision(12)<<"scheme,api,pattern,length,repeat,iterations,wall_ns,cpu_ns,cpu_ns_per_event,checksum,alloc_calls,free_calls,alloc_bytes\n";
 for(bool legacy:{false,true})for(unsigned scheme=2;scheme<6;scheme++)for(size_t length:{12,64,512})for(int rep=0;rep<5;rep++){
  PBCase c(scheme,3,length,legacy);
  auto run=[&](uint64_t i){auto encode=[&](auto&parent){
   if(scheme%2==0)parent.Clear();else{parent.clear_common();parent.clear_tls();parent.clear_http();parent.clear_dns();}
   parent.set_event_id(i+1);auto*common=parent.mutable_common();common->set_value0(i+5);common->set_value1(i*9+7);
   c.fill(parent.mutable_tls(),i);c.fill(parent.mutable_http(),i);
   size_t n=parent.ByteSizeLong();parent.SerializeWithCachedSizesToArray(c.wire.data());escape(c.wire.data());return n;
  };return scheme<4?encode(c.parallel):encode(c.implicit);};
  for(size_t i=0;i<64;i++){size_t n=c.run(i);std::string original(reinterpret_cast<char*>(c.wire.data()),n);size_t got=run(i);require(original==std::string_view(reinterpret_cast<char*>(c.wire.data()),got),"corrected mixed wire mismatch");}
  uint64_t count=4096,sum=0;
#ifdef ALLOC_AUDIT
  count=10000;alloc_calls=free_calls=alloc_bytes=0;counting=true;
#endif
  for(;;){auto cpu0=clock_ns(CLOCK_THREAD_CPUTIME_ID),wall0=clock_ns();sum=0;for(uint64_t i=0;i<count;i++)sum+=run(i);auto wall=clock_ns()-wall0,cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;
#ifdef ALLOC_AUDIT
   counting=false;auto ac=alloc_calls,fc=free_calls,ab=alloc_bytes;
#else
   uint64_t ac=0,fc=0,ab=0;if(wall<80000000){count=uint64_t(double(count)*88000000/std::max(wall,uint64_t(1))/4096+1)*4096;continue;}
#endif
   std::cout<<scheme_name(scheme)<<','<<(legacy?"set_ptr_len":"mutable_assign")<<",mixed_tls_http_P2_only,"<<length<<','<<rep<<','<<count<<','<<wall<<','<<cpu<<','<<double(cpu)/count<<','<<sum<<','<<ac<<','<<fc<<','<<ab<<'\n';std::cout.flush();break;
  }
 }
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
