#define main original_codec_main
#include "bench.cpp"
#undef main
__attribute__((noinline)) uint64_t consume_flat(const void*buf){
 auto m=fbbench::GetEvent(buf);auto p=m->payload();return m->id()+m->seq()+m->timestamp()+m->value0()+m->value1()+m->producer()+m->stream()+m->type()+m->flags()+sum_payload(reinterpret_cast<const char*>(p->data()),p->size());
}
int main(){cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(0,&cpus);std::cerr<<"pin_cpu0="<<sched_setaffinity(0,sizeof(cpus),&cpus)<<"\n";
 std::cout<<"codec,payload_bytes,pattern,operation,repeat,wire_bytes_mean,iterations,ns_per_op,checksum\n";
 for(size_t n:{0,64,256,1024})for(std::string pattern:{"random","repetitive"}) {
  make_events(n,pattern);Flat flat;std::string out;out.reserve(2048);double bytes=0;
  for(size_t i=0;i<corpus_n;i++){bytes+=flat.build_no_copy(events[i]);auto got=consume_flat(flat.builder.GetBufferPointer());auto want=sum_header(events[i].h,true)+sum_payload(events[i].payload.data(),n);if(got!=want)std::abort();}
  for(int rep=0;rep<5;rep++)for(std::string op:{"encode_no_output_copy","encode_with_output_copy","roundtrip_no_output_copy"}){
   Measurement m;if(op=="encode_no_output_copy")m=measure(op,[&](size_t i){return flat.build_no_copy(events[i]);});
   else if(op=="encode_with_output_copy")m=measure(op,[&](size_t i){return flat.encode(events[i],out);});
   else m=measure(op,[&](size_t i){flat.build_no_copy(events[i]);return consume_flat(flat.builder.GetBufferPointer());});
   std::cout<<"flatbuffers_builder_direct,"<<n<<","<<pattern<<","<<op<<","<<rep<<","<<bytes/corpus_n<<","<<m.iterations<<","<<m.ns<<","<<m.checksum<<"\n";
  }
 }
}
