#include "common.hpp"
#include "plan_compiler.hpp"
#include "pb_adapters.hpp"
#ifdef HAVE_CUSTOM
#include "custom/codecs.hpp"
#endif
#ifdef HAVE_UPB
#include "upb/adapter.hpp"
#endif
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sched.h>
#include <sstream>
#include <time.h>
#include <sys/utsname.h>
#include <filesystem>
using namespace dynbench;
constexpr size_t CORPUS_N=4096;
volatile uint64_t observable_checksum=0;
uint64_t clock_ns(clockid_t id=CLOCK_MONOTONIC){timespec t;clock_gettime(id,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
std::string read_file(const std::string&p){std::ifstream f(p);if(!f)throw std::runtime_error("cannot open "+p);std::ostringstream s;s<<f.rdbuf();return s.str();}
std::vector<Event> make_events(size_t bytes,const std::string&pattern){
 std::mt19937_64 rng(0x61aba1+uint64_t(bytes));std::vector<Event> events;events.reserve(CORPUS_N);
 for(size_t i=0;i<CORPUS_N;i++){
  Event e;e.h={rng(),uint64_t(i+1),1770000000000000000ULL+i*17,rng(),rng(),uint32_t(1+i%32),uint32_t(1+i%64),uint32_t(1+i%7),uint32_t(i%16),64,1,uint32_t(bytes)};
  e.payload.resize(bytes);constexpr char text[]="INFO device=worker metric=rx_packets status=ok value=1234567890 ";
  for(size_t j=0;j<bytes;j++)e.payload[j]=pattern=="random"?char(rng()):text[(i+j)%(sizeof(text)-1)];events.push_back(std::move(e));
 }return events;
}
struct CodecEntry{std::unique_ptr<Codec>codec;bool static_full=false;uint64_t cold_init_ns=0;bool sensitivity=false;};
std::vector<CodecEntry> make_codecs(){
 std::vector<CodecEntry> v;
 auto add=[&](auto create,bool static_full,bool sensitivity=false){uint64_t start=clock_ns();std::unique_ptr<Codec>c=create();uint64_t elapsed=clock_ns()-start;v.push_back({std::move(c),static_full,elapsed,sensitivity});};
 add([]{return std::make_unique<GeneratedPB<bench::Event,true>>();},true);
 add([]{return std::make_unique<GeneratedPB<bench::FixedEvent,true>>();},true);
 add([]{return std::make_unique<GeneratedPB<bench::Event,false>>();},false);
 add([]{return std::make_unique<GeneratedPB<bench::FixedEvent,false>>();},false);
 add([]{return std::make_unique<ReflectionPB>(false);},false);add([]{return std::make_unique<ReflectionPB>(true);},false);
#ifdef HAVE_CUSTOM
 add([]{return std::make_unique<HybridCodec>();},false);add([]{return std::make_unique<ProtoEmitCodec>(false);},false);add([]{return std::make_unique<ProtoEmitCodec>(true);},false);
#endif
#ifdef HAVE_UPB
 for(bool fixed:{false,true})for(bool alias:{false,true})add([fixed,alias]{return make_upb_codec(fixed,alias);},false);
 for(bool fixed:{false,true})add([fixed]{return make_upb_codec_ex({fixed,false,true,false});},false,true);
#endif
 return v;
}
struct InputCase{std::string profile,pattern="random";size_t payload=0;std::shared_ptr<std::vector<Event>>events;};
struct Job{Codec*codec;InputCase*input;std::vector<std::string>wires;std::string output;double wire_mean=0;bool sensitivity=false;std::array<const Plan*,CORPUS_N> plan_refs{};};
struct Plans{std::vector<Plan>values;size_t full,subset,no_payload;const Plan&get(std::string_view profile,size_t i)const{
 if(profile=="mixed"){size_t k=i%3;return values[k==0?full:k==1?subset:no_payload];}
 if(profile=="full")return values[full];if(profile=="subset")return values[subset];if(profile=="no_payload")return values[no_payload];throw std::invalid_argument("bad profile");
}};
template<class PB>void validate_pb(std::string_view w,const Event&e,const Plan&p){
 PB m;if(!m.ParseFromArray(w.data(),int(w.size())))throw std::runtime_error("official PB rejects wire");
 auto want=[&](int fid,uint64_t x){return p.has(fid)?x:0;};
 bool ok=m.id()==want(1,e.h.id)&&m.seq()==want(2,e.h.seq)&&m.timestamp()==want(3,e.h.timestamp)&&m.value0()==want(4,e.h.value0)&&m.value1()==want(5,e.h.value1)&&m.producer()==want(6,e.h.producer)&&m.stream()==want(7,e.h.stream)&&m.type()==want(8,e.h.type)&&m.flags()==want(9,e.h.flags)&&m.payload()==(p.has(10)?e.payload:std::string());
 if(!ok)throw std::runtime_error("official PB field/payload mismatch");
}
void validate_wire(Codec&c,std::string_view w,const Event&e,const Plan&p){
 if(c.decode(w,false)!=expected_checksum(e,p,false)||c.decode(w,true)!=expected_checksum(e,p,true))throw std::runtime_error("checksum mismatch "+std::string(c.name()));
 std::string name=c.name();if(name.find("hybrid")!=std::string::npos)return;
 if(name.find("fixed")!=std::string::npos)validate_pb<bench::FixedEvent>(w,e,p);else validate_pb<bench::Event>(w,e,p);
}
void prepare(Job&job,const Plans&plans){
 job.wires.resize(CORPUS_N);uint64_t total=0;
 for(size_t i=0;i<CORPUS_N;i++){const auto&e=(*job.input->events)[i];const auto&p=plans.get(job.input->profile,i);job.plan_refs[i]=&p;total+=job.codec->encode(e,p,job.wires[i]);validate_wire(*job.codec,job.wires[i],e,p);}
 job.wire_mean=double(total)/CORPUS_N;job.output.reserve(job.input->payload+256);
}
struct Measurement{uint64_t iterations,checksum,elapsed_ns,cpu_ns;double ns;};
template<class Fn> Measurement measure(Fn&&fn,uint64_t minimum_ns){
 uint64_t warm=0;for(size_t i=0;i<CORPUS_N;i++)warm+=fn(i);observable_checksum=warm;
 uint64_t count=CORPUS_N;
 for(;;){
  uint64_t checksum=0,cpu0=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns();
  for(uint64_t i=0;i<count;i++)checksum+=fn(size_t(i&(CORPUS_N-1)));
  uint64_t elapsed=clock_ns()-start,cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu0;observable_checksum=checksum;
  if(elapsed>=minimum_ns)return{count,checksum,elapsed,cpu,double(elapsed)/count};
  long double desired=std::ceil((long double)count*minimum_ns*1.15L/std::max(uint64_t(1),elapsed)/CORPUS_N)*CORPUS_N;
  if(desired>uint64_t(-1)/4||desired<=count)throw std::runtime_error("calibration iteration overflow");
  count=uint64_t(desired); // Always execute the final reported count; no unexecuted cap step.
 }
}
std::string b64(std::string_view s){constexpr char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string o;for(size_t i=0;i<s.size();i+=3){uint32_t x=uint8_t(s[i])<<16;if(i+1<s.size())x|=uint8_t(s[i+1])<<8;if(i+2<s.size())x|=uint8_t(s[i+2]);o+=t[(x>>18)&63];o+=t[(x>>12)&63];o+=i+1<s.size()?t[(x>>6)&63]:'=';o+=i+2<s.size()?t[x&63]:'=';}return o;}
void fixtures(const std::string&path,std::vector<CodecEntry>&codecs,const Plans&plans){
 std::ofstream f(path);if(!f)throw std::runtime_error("cannot write fixtures");
 std::vector<Event>edge(3);edge[0].h.header_len=64;edge[0].h.version=1;
 edge[1].h={0x8000000000000001ULL,UINT64_MAX,1,UINT64_MAX-1,0,UINT32_MAX,0,0x80000000U,42,64,1,5};edge[1].payload=std::string("\0\xff\x80" "A\0",5);
 edge[2]=make_events(64,"random")[7];
 size_t count=0;
 for(auto&entry:codecs)for(const std::string profile:{"full","subset","no_payload","full"}){
  if(entry.static_full&&profile!="full")continue;
  for(size_t i=0;i<edge.size();i++){
   const auto&e=edge[i];const auto&p=plans.get(profile,i);std::string wire;entry.codec->encode(e,p,wire);validate_wire(*entry.codec,wire,e,p);
   nlohmann::json expected;for(const auto&meta:registry){if(meta.fid==10)expected["payload_base64"]=b64(p.has(10)?std::string_view(e.payload):std::string_view());
    else{uint64_t v=0;if(p.has(meta.fid)){FieldOp op;op.kind=meta.kind;op.source_offset=meta.offset;v=scalar_value(e,op);}expected[meta.name]=std::to_string(v);}}
   std::string name=entry.codec->name();std::string type=name.find("hybrid")!=std::string::npos?"Hybrid":name.find("fixed")!=std::string::npos?"FixedEvent":"Event";
   nlohmann::json row={{"case",i==0?"zero_defaults":i==1?"u64_highbit_max_bytes":"random64"},{"codec",name},{"plan",profile},{"expected",expected},{"wire_base64",b64(wire)},{"protobuf_type",type}};
   f<<row.dump()<<'\n';++count;
  }
 }
 std::cerr<<"fixtures="<<count<<" path="<<path<<"\n";
}
void plan_self_tests(){
 const std::vector<std::string>bad={
  R"({"version":1,"profiles":[{"name":"x","fields":["id","id"]}]})",
  R"({"version":1,"profiles":[{"name":"x","fields":["unknown"]}]})",
  R"({"version":1,"profiles":[{"name":"x","fields":[{"name":"id","type":"u32"}]}]})",
  R"({"version":1,"profiles":[{"name":"x","fields":[{"name":"id","fid":2}]}]})",
  R"({"version":1,"profiles":[{"name":"x","fields":[{"name":"id","offset":8}]}]})",
  R"({"version":1,"profiles":[{"name":"x","fields":["id"]},{"name":"x","fields":["seq"]}]})"};
 for(const auto&s:bad){bool rejected=false;try{auto x=compile_config(s);(void)x;}catch(const std::exception&){rejected=true;}if(!rejected)throw std::runtime_error("invalid config accepted");}
}
int main(int argc,char**argv){try{
 std::string config="plans.json",outdir="results";bool self_test=false,fixtures_only=false;int repeats=5;uint64_t min_ns=100000000;
 for(int a=1;a<argc;a++){std::string x=argv[a];if(x=="--self-test")self_test=true;else if(x=="--fixtures-only")fixtures_only=true;else if(x=="--config"&&a+1<argc)config=argv[++a];else if(x=="--output"&&a+1<argc)outdir=argv[++a];else if(x=="--repeats"&&a+1<argc)repeats=std::stoi(argv[++a]);else if(x=="--min-ms"&&a+1<argc)min_ns=uint64_t(std::stod(argv[++a])*1e6);else throw std::invalid_argument("unknown argument "+x);}
 if(!self_test&&!fixtures_only&&min_ns<100000000)throw std::invalid_argument("official performance runs require >=100ms");
 cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(0,&cpus);if(sched_setaffinity(0,sizeof(cpus),&cpus))throw std::runtime_error("cannot pin CPU0");
 std::filesystem::create_directories(outdir);
 auto text=read_file(config);uint64_t cold0=clock_ns();Plans plans;plans.values=compile_config(text);plans.full=plan_index(plans.values,"full");plans.subset=plan_index(plans.values,"subset");plans.no_payload=plan_index(plans.values,"no_payload");uint64_t cold_ns=clock_ns()-cold0;
 plan_self_tests();
#ifdef HAVE_CUSTOM
 for(const auto&p:plans.values)validate_custom_plan(p);
#endif
 auto codecs=make_codecs();fixtures(outdir+"/fixtures.jsonl",codecs,plans);
 if(fixtures_only)return 0;
 std::vector<InputCase> inputs;for(size_t n:{0,64,256,1024})inputs.push_back({"full","random",n,std::make_shared<std::vector<Event>>(make_events(n,"random"))});
 for(std::string profile:{"subset","no_payload","mixed"})inputs.push_back({profile,"random",256,std::make_shared<std::vector<Event>>(make_events(256,"random"))});
 std::vector<Job>jobs;
 for(auto&ce:codecs)for(auto&input:inputs){if(ce.static_full&&input.profile!="full")continue;if(ce.sensitivity&&(input.payload!=256||input.profile=="no_payload"))continue;jobs.push_back({ce.codec.get(),&input,{},{},0,ce.sensitivity});prepare(jobs.back(),plans);}
 std::cerr<<"validation passed jobs="<<jobs.size()<<" codecs="<<codecs.size()<<" corpus="<<CORPUS_N<<"\n";
 nlohmann::json metadata={{"corpus_n",CORPUS_N},{"seed_formula","0x61aba1+payload_bytes (identical to original codec/bench.cpp)"},{"minimum_run_ns",min_ns},{"repeats",repeats},{"cpu",0},{"protobuf_version",GOOGLE_PROTOBUF_VERSION},{"cold_parse_compile_once_ns",cold_ns},{"plans",plans.values.size()},{"codecs",codecs.size()},{"jobs",jobs.size()},{"checksum_semantics","few=id+seq+type; all=all present scalar sum+payload length+first+last. Functional official protobuf verification additionally compares every field and all payload bytes."},{"plan_boundary","JSON chooses known source registry fields; cannot add arbitrary business fields or offsets. mixed references one of three immutable plans per record, no runtime code compilation."},{"timing_boundary","encode includes Clear/set(selected fields)/serialize/output std::string write; output capacity reused. decode includes parsing+field consumption. output owning/alias and Arena policy encoded in codec name/docs."}};
 metadata["compiler"]=__VERSION__;metadata["codec_init_once_ns"]=nlohmann::json::array();
 for(const auto&entry:codecs)metadata["codec_init_once_ns"].push_back({{"encoder",entry.codec->name()},{"elapsed_ns",entry.cold_init_ns}});
 {std::ofstream m(outdir+"/metadata.json");m<<metadata.dump(2)<<'\n';}
 if(self_test)return 0;
 struct Task{size_t job;std::string op;};std::vector<Task>tasks;
 for(size_t j=0;j<jobs.size();j++){
  if(jobs[j].sensitivity){tasks.push_back({j,"encode"});continue;}
  for(std::string op:{"encode","decode_all","decode_few"})tasks.push_back({j,op});
  if(jobs[j].input->payload==256&&jobs[j].input->profile!="no_payload")tasks.push_back({j,"roundtrip"});
 }
 std::ofstream f(outdir+"/benchmark.csv");f<<"encoder,profile,payload_bytes,pattern,operation,repeat,wire_bytes_mean,ns_per_op,checksum,iterations,elapsed_ns,cpu_ns,cpu_ns_per_op,order_index\n";f<<std::setprecision(12);
 std::mt19937 order(20261029);uint64_t order_index=0;
 for(int rep=0;rep<repeats;rep++){
  std::shuffle(tasks.begin(),tasks.end(),order);
  for(const auto&task:tasks){auto&j=jobs[task.job];auto&c=*j.codec;auto&input=*j.input;auto&events=*input.events;Measurement m;
   if(task.op=="encode")m=measure([&](size_t i){c.encode(events[i],*j.plan_refs[i],j.output);return uint64_t(j.output.size())+(j.output.empty()?0:uint8_t(j.output.front())+uint8_t(j.output.back()));},min_ns);
   else if(task.op=="roundtrip")m=measure([&](size_t i){c.encode(events[i],*j.plan_refs[i],j.output);return c.decode(j.output,false);},min_ns);
   else{bool few=task.op=="decode_few";m=measure([&](size_t i){return c.decode(j.wires[i],few);},min_ns);}
   f<<c.name()<<','<<input.profile<<','<<input.payload<<','<<input.pattern<<','<<task.op<<','<<rep<<','<<j.wire_mean<<','<<m.ns<<','<<m.checksum<<','<<m.iterations<<','<<m.elapsed_ns<<','<<m.cpu_ns<<','<<double(m.cpu_ns)/m.iterations<<','<<order_index++<<'\n';f.flush();
  }
  std::cerr<<"outer repeat "<<rep<<" complete rows="<<order_index<<"\n";
 }
 std::ofstream p(outdir+"/plan_compile.csv");p<<"operation,repeat,iterations,elapsed_ns,ns_per_op,checksum\n";
 for(int rep=0;rep<repeats;rep++){
  auto m=measure([&](size_t i){auto compiled=compile_config(text);return uint64_t(compiled[i%compiled.size()].mask+compiled.size());},min_ns);
  p<<"json_parse_validate_compile_3_plans,"<<rep<<','<<m.iterations<<','<<m.elapsed_ns<<','<<std::setprecision(12)<<m.ns<<','<<m.checksum<<'\n';
  m=measure([&](size_t i){const auto&plan=plans.get("mixed",i);escape(&plan);return uint64_t(plan.mask);},min_ns);
  p<<"hot_mixed_plan_reference_selection,"<<rep<<','<<m.iterations<<','<<m.elapsed_ns<<','<<std::setprecision(12)<<m.ns<<','<<m.checksum<<'\n';
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 2;}}
