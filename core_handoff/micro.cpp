#include "common.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sched.h>
#include <time.h>
using namespace handoff;
uint64_t micro_ns(clockid_t c=CLOCK_MONOTONIC){timespec t;clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
volatile uint64_t micro_observable=0;
enum class Stage{Extract,Native,Scratch,EncodePB,FillPBObject,LoggerEncodeNative,LoggerSerializeObject,LoggerCopyWire,LoggerDirectWire};
const char*stage_name(Stage s){switch(s){case Stage::Extract:return"worker_extract_only";case Stage::Native:return"worker_native_direct";case Stage::Scratch:return"worker_native_scratch_copy";case Stage::EncodePB:return"worker_pb_encode";case Stage::FillPBObject:return"worker_fill_pb_object";case Stage::LoggerEncodeNative:return"logger_encode_native";case Stage::LoggerSerializeObject:return"logger_serialize_pb_object";case Stage::LoggerCopyWire:return"logger_copy_wire";case Stage::LoggerDirectWire:return"logger_direct_wire";}return"invalid";}
struct alignas(64) Wire{std::array<uint8_t,MAX_WIRE>data;size_t size=0;};
struct Context{
 Shape shape;size_t tail;
 std::vector<Packet>packets;std::vector<NativeSlot>native;
 std::vector<Wire>input_wire,output;
 std::vector<std::unique_ptr<PBState>>objects;
 PBState tls;NativeSlot scratch;
 Context(Shape sh,size_t t):shape(sh),tail(t),packets(make_corpus(t)),native(CORPUS_N),input_wire(CORPUS_N),output(CORPUS_N){
  objects.reserve(CORPUS_N);
  for(size_t i=0;i<CORPUS_N;i++){
   auto v=extract(packets[i]);fill_slot(native[i],v);objects.push_back(std::make_unique<PBState>());fill_pb(*objects.back(),shape,v);
   input_wire[i].size=encode_pb(tls,shape,v,input_wire[i].data.data());
   if(!verify_wire(shape,std::string_view(reinterpret_cast<char*>(input_wire[i].data.data()),input_wire[i].size),v))throw std::runtime_error("micro initial wire invalid");
   if(!verify_wire(shape,std::string_view(reinterpret_cast<char*>(input_wire[i].data.data()),input_wire[i].size),view_slot(native[i])))throw std::runtime_error("micro initial native invalid");
  }
 }
};
struct Job{Context*ctx;Stage stage;bool pressure;uint64_t cursor=0x895aac12;};
std::vector<uint64_t>flow;
__attribute__((noinline)) uint64_t flow_work(uint64_t cursor,uint64_t salt){
 for(int i=0;i<8;i++){cursor=flow[(cursor^salt)&(flow.size()-1)];salt+=0x9e3779b97f4a7c15ULL;}
 return cursor;
}
__attribute__((noinline)) uint64_t execute(Job&j,size_t i){
 auto&c=*j.ctx;uint64_t result=0;
 if(j.stage<=Stage::FillPBObject){
  auto s=extract(c.packets[i]);
  result=s.h.id+s.h.seq+s.h.timestamp+s.h.value0+s.h.value1+s.h.producer+s.h.stream+s.h.type+s.h.flags+s.flat.size;
  if(j.pressure){j.cursor=flow_work(j.cursor,s.h.id);result+=j.cursor;}
  if(j.stage==Stage::Native){fill_slot(c.native[i],s);escape(&c.native[i]);result+=c.native[i].h.seq;}
  else if(j.stage==Stage::Scratch){fill_slot(c.scratch,s);std::memcpy(&c.native[i],&c.scratch,offsetof(NativeSlot,tail)+s.flat.size);escape(&c.native[i]);result+=c.native[i].h.seq;}
  else if(j.stage==Stage::EncodePB){auto&out=c.output[i];out.size=encode_pb(c.tls,c.shape,s,out.data.data());escape(out.data.data());result+=wire_checksum(out.data.data(),out.size);}
  else if(j.stage==Stage::FillPBObject){fill_pb(*c.objects[i],c.shape,s);escape(c.objects[i].get());result+=s.h.seq;}
 }else{
  auto&out=c.output[i];
  if(j.stage==Stage::LoggerEncodeNative){out.size=encode_pb(c.tls,c.shape,view_slot(c.native[i]),out.data.data());escape(out.data.data());result=wire_checksum(out.data.data(),out.size);}
  else if(j.stage==Stage::LoggerSerializeObject){out.size=serialize_pb(*c.objects[i],c.shape,out.data.data(),MAX_WIRE);escape(out.data.data());result=wire_checksum(out.data.data(),out.size);}
  else if(j.stage==Stage::LoggerCopyWire){out.size=c.input_wire[i].size;std::memcpy(out.data.data(),c.input_wire[i].data.data(),out.size);escape(out.data.data());result=wire_checksum(out.data.data(),out.size);}
  else result=wire_checksum(c.input_wire[i].data.data(),c.input_wire[i].size);
 }
 return result;
}
struct Measure{uint64_t n,elapsed,cpu,sum;};
Measure measure(Job&j){
 uint64_t sum=0;for(size_t i=0;i<CORPUS_N;i++)sum+=execute(j,i);micro_observable=sum;uint64_t n=CORPUS_N;
 for(;;){sum=0;auto cpu0=micro_ns(CLOCK_THREAD_CPUTIME_ID),start=micro_ns();for(uint64_t i=0;i<n;i++)sum+=execute(j,i&(CORPUS_N-1));auto elapsed=micro_ns()-start,cpu=micro_ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;micro_observable=sum;
  if(elapsed>=80000000)return{n,elapsed,cpu,sum};
  n=uint64_t(double(n)*80000000.0/double(std::max(uint64_t(1),elapsed))*1.12/CORPUS_N+1)*CORPUS_N;
 }
}
int main(){try{
 cpu_set_t s;CPU_ZERO(&s);CPU_SET(0,&s);if(sched_setaffinity(0,sizeof(s),&s))throw std::runtime_error("CPU0 pin failed");
 flow.resize(1u<<22);std::mt19937_64 rng(20261029);for(auto&v:flow)v=rng();
 std::vector<std::unique_ptr<Context>>contexts;std::vector<Job>jobs;
 for(Shape shape:{Shape::Flat,Shape::Nested})for(size_t tail:{0,64,256,1024}){
  contexts.push_back(std::make_unique<Context>(shape,tail));auto*c=contexts.back().get();
  for(Stage st:{Stage::Extract,Stage::Native,Stage::Scratch,Stage::EncodePB,Stage::FillPBObject,Stage::LoggerEncodeNative,Stage::LoggerSerializeObject,Stage::LoggerCopyWire,Stage::LoggerDirectWire}){
   jobs.push_back({c,st,false});if(st<=Stage::FillPBObject)jobs.push_back({c,st,true});
  }
 }
 std::ofstream f("results/micro.csv");f<<"schema,tail_bytes,workload,stage,repeat,iterations,elapsed_ns,cpu_ns,wall_ns_per_event,cpu_ns_per_event,checksum,order_index\n"<<std::setprecision(12);
 std::vector<size_t>order(jobs.size());std::iota(order.begin(),order.end(),0);std::mt19937 shuffle(20261029);size_t oi=0;
 for(int rep=0;rep<5;rep++){std::shuffle(order.begin(),order.end(),shuffle);for(size_t ix:order){auto&j=jobs[ix];auto m=measure(j);f<<shape_name(j.ctx->shape)<<','<<j.ctx->tail<<','<<(j.pressure?"synthetic_32MiB_8dependent_reads":"hot")<<','<<stage_name(j.stage)<<','<<rep<<','<<m.n<<','<<m.elapsed<<','<<m.cpu<<','<<double(m.elapsed)/m.n<<','<<double(m.cpu)/m.n<<','<<m.sum<<','<<oi++<<'\n';f.flush();}std::cerr<<"micro repeat "<<rep<<" rows="<<oi<<'\n';}
 std::cout<<"PASS jobs="<<jobs.size()<<" rows="<<oi<<" no_queue_cpu_service_only; flow model is synthetic, not DPI\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
