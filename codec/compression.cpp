#include <zstd.h>
#include <lz4.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <sched.h>
using Clock=std::chrono::steady_clock;
volatile uint64_t checksum_out=0;
inline void escape(const void* p){asm volatile(""::"g"(p):"memory");}
__attribute__((noinline)) size_t copy_bytes(char*o,const char*i,size_t n){memcpy(o,i,n);escape(o);return n;}
template<class F>void run(const char*format,const char*op,size_t record,size_t batch,const std::string&pattern,int rep,size_t input_bytes,double output_bytes,F&&fn){
 uint64_t sum=0;for(size_t i=0;i<1024;i++)sum+=fn(i%256);size_t count=1024;double elapsed;
 do{auto a=Clock::now();for(size_t i=0;i<count;i++)sum+=fn(i%256);elapsed=std::chrono::duration<double,std::nano>(Clock::now()-a).count();if(elapsed>=20000000. || count>=(1u<<24))break;count*=2;}while(true);checksum_out=sum;
 std::cout<<format<<","<<record<<","<<batch<<","<<pattern<<","<<op<<","<<rep<<","<<input_bytes<<","<<output_bytes<<","<<count<<","<<elapsed/count/batch<<","<<sum<<"\n";
}
int main(){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(0,&cpus);std::cerr<<"pin_cpu0="<<sched_setaffinity(0,sizeof(cpus),&cpus)<<" zstd="<<ZSTD_versionString()<<" lz4="<<LZ4_versionString()<<"\n";
 std::cout<<"codec,record_bytes,batch,pattern,operation,repeat,input_bytes_batch,output_bytes_batch_mean,iterations,ns_per_record,checksum\n";
 for(size_t record:{128,320,1088})for(size_t batch:{1,32})for(std::string pattern:{"random","repetitive"}) {
  size_t n=record*batch;std::mt19937_64 rng(61004+n);std::vector<std::string> in(256,std::string(n,' ')),lz(256),zs(256);
  for(size_t i=0;i<256;i++)for(size_t k=0;k<n;k++)in[i][k]=((k%record)<64||pattern=="random")?char(rng()):"INFO metric=rx_packets status=ok device=worker value=12345 "[(k+i)%(sizeof("INFO metric=rx_packets status=ok device=worker value=12345 ")-1)];
  size_t cap=std::max(size_t(LZ4_compressBound(int(n))),ZSTD_compressBound(n));std::vector<char> tmp(cap),out(n);double lzbytes=0,zsbytes=0;
  for(size_t i=0;i<256;i++){
   int a=LZ4_compress_default(in[i].data(),tmp.data(),int(n),int(cap));if(a<=0)return 2;lz[i].assign(tmp.data(),a);lzbytes+=a;
   int l=LZ4_decompress_safe(lz[i].data(),out.data(),a,int(n));if(l!=int(n)||memcmp(out.data(),in[i].data(),n))return 3;
   auto b=ZSTD_compress(tmp.data(),cap,in[i].data(),n,1);if(ZSTD_isError(b))return 4;zs[i].assign(tmp.data(),b);zsbytes+=b;
   auto z=ZSTD_decompress(out.data(),n,zs[i].data(),b);if(z!=n||memcmp(out.data(),in[i].data(),n))return 5;
  }
  for(int rep=0;rep<5;rep++){
   run("memcpy","copy",record,batch,pattern,rep,n,n,[&](size_t i){return copy_bytes(out.data(),in[i].data(),n);});
   run("lz4","compress",record,batch,pattern,rep,n,lzbytes/256,[&](size_t i){auto z=LZ4_compress_default(in[i].data(),tmp.data(),int(n),int(cap));escape(tmp.data());return z;});
   run("lz4","decompress",record,batch,pattern,rep,n,lzbytes/256,[&](size_t i){auto z=LZ4_decompress_safe(lz[i].data(),out.data(),int(lz[i].size()),int(n));escape(out.data());return z;});
   run("zstd_level1","compress",record,batch,pattern,rep,n,zsbytes/256,[&](size_t i){auto z=ZSTD_compress(tmp.data(),cap,in[i].data(),n,1);escape(tmp.data());return z;});
   run("zstd_level1","decompress",record,batch,pattern,rep,n,zsbytes/256,[&](size_t i){auto z=ZSTD_decompress(out.data(),n,zs[i].data(),zs[i].size());escape(out.data());return z;});
  }
 }
}
