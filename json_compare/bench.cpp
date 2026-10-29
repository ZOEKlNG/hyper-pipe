#include "benchmark_common.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sched.h>
#include <time.h>
using namespace jsonbench;
uint64_t now(clockid_t c=CLOCK_MONOTONIC){timespec t;clock_gettime(c,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
volatile uint64_t observable=0;
enum class Stage{Prepared,Fields,Parse,Decode,RawEncode,RawDecode};
const char*stage_name(Stage s){switch(s){case Stage::Prepared:return"encode_prepared";case Stage::Fields:return"encode_fields";case Stage::Parse:return"parse_only";case Stage::Decode:return"decode_fields";case Stage::RawEncode:return"encode_raw";case Stage::RawDecode:return"decode_raw";}return"unknown";}
struct Config{Shape shape;size_t tail;bool binary;size_t lib;};
struct Context {
  Config cfg;
  std::vector<Record>raw,repr;
  std::vector<std::unique_ptr<Codec>>prepared;
  std::vector<std::string>wire;
  std::unique_ptr<Codec>codec;
  Record temporary,result,restored;
  double encoded_bytes=0,decode_bytes=0;
  explicit Context(Config c):cfg(c),raw(make_records(c.shape,c.tail,c.binary)),repr(raw),codec(factories[c.lib](c.shape)){
    prepared.reserve(CORPUS_N);wire.reserve(CORPUS_N);
    for(size_t i=0;i<CORPUS_N;i++){
      if(c.binary&&c.lib!=0)transform(raw[i],repr[i],c.shape,true);
      prepared.push_back(factories[c.lib](c.shape));prepared.back()->load(repr[i]);
      auto encoded=prepared.back()->encode();encoded_bytes+=encoded.size();
      // All JSON readers receive the exact same canonical document bytes and
      // field order, independent of their encoder's map iteration order.
      wire.push_back(c.lib==0?std::string(encoded):json_record(repr[i],c.shape).dump());decode_bytes+=wire.back().size();
    }
    encoded_bytes/=CORPUS_N;decode_bytes/=CORPUS_N;
    codec->load(repr.back());codec->encode();codec->parse(wire.back());codec->read_back(result);
  }
};
__attribute__((noinline)) uint64_t execute(Context&c,Stage stage,size_t i){
  auto&codec=*c.codec;
  if(stage==Stage::Prepared){auto s=c.prepared[i]->encode();escape(s.data());return witness(s);}
  if(stage==Stage::Fields){codec.load(c.repr[i]);auto s=codec.encode();escape(s.data());return witness(s);}
  if(stage==Stage::RawEncode){
    if(c.cfg.lib==0)codec.load(c.raw[i]);else{transform(c.raw[i],c.temporary,c.cfg.shape,true);codec.load(c.temporary);}
    auto s=codec.encode();escape(s.data());return witness(s);
  }
  codec.parse(c.wire[i]);
  if(stage==Stage::Parse){escape(codec.parsed_state());return c.wire[i].size();}
  codec.read_back(c.result);
  if(stage==Stage::RawDecode&&c.cfg.lib!=0){transform(c.result,c.restored,c.cfg.shape,false);escape(&c.restored);return witness(c.restored);}
  escape(&c.result);return witness(c.result);
}
struct Measurement{uint64_t n,wall,cpu,checksum;};
Measurement measure(Context&c,Stage stage,uint64_t min_ns){
  uint64_t checksum=0;for(size_t i=0;i<CORPUS_N;i++)checksum+=execute(c,stage,i);observable=checksum;
  uint64_t n=CORPUS_N;
  for(;;){checksum=0;auto cpu0=now(CLOCK_THREAD_CPUTIME_ID),wall0=now();for(uint64_t i=0;i<n;i++)checksum+=execute(c,stage,i&(CORPUS_N-1));auto wall=now()-wall0,cpu=now(CLOCK_THREAD_CPUTIME_ID)-cpu0;observable=checksum;
    if(wall>=min_ns)return {n,wall,cpu,checksum};
    n=uint64_t(double(n)*double(min_ns)/double(std::max(wall,uint64_t(1)))*1.1/CORPUS_N+1)*CORPUS_N;
  }
}
void require(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
void smoke(){
  size_t records=0,cross=0,negative=0,ownership=0,b64=0;Record got,back;
  const std::array<std::string_view,7>rfc={"","f","fo","foo","foob","fooba","foobar"};
  const std::array<std::string_view,7>expected={"","Zg==","Zm8=","Zm9v","Zm9vYg==","Zm9vYmE=","Zm9vYmFy"};
  for(size_t i=0;i<rfc.size();i++){std::string out,decoded;base64_encode(rfc[i],out);require(out==expected[i],"RFC Base64 encode");base64_decode(out,decoded);require(decoded==rfc[i],"RFC Base64 decode");b64++;}
  for(auto bad:{"A","====","A===","Zg=Z","Zh==","Zm9=","Zg==AAAA","Zg$=","Zg==\n"}){bool fail=false;try{std::string out;base64_decode(bad,out);}catch(...){fail=true;}require(fail,"bad Base64 accepted");b64++;}
  for(Shape shape:{Shape::Flat,Shape::Nested})for(size_t tail:{0,64,256,1024})for(bool binary:{false,true}){
    auto input=make_records(shape,tail,binary);
    std::array<std::unique_ptr<Codec>,4>codecs;for(size_t k=0;k<4;k++)codecs[k]=factories[k](shape);
    for(size_t i=0;i<128;i++){
      auto raw=input[i];if(i==0)raw.h={UINT64_MAX,UINT64_C(9007199254740993),UINT64_C(9223372036854775808),0,UINT64_C(18446744073709551614),UINT32_MAX,0,UINT32_MAX,0};
      if(i==1)raw.h={};
      Record representation=raw;if(binary)transform(raw,representation,shape,true);
      for(size_t encoder=0;encoder<4;encoder++){
        auto&c=*codecs[encoder];const auto&expected_record=encoder==0?raw:representation;
        c.load(expected_record);std::string encoded(c.encode());
        c.parse(encoded);c.read_back(got);require(got==expected_record,"own roundtrip mismatch");records++;
        if(binary&&encoder!=0){transform(got,back,shape,false);require(back==raw,"binary roundtrip mismatch");}
        if(encoder!=0)for(size_t reader=1;reader<4;reader++){codecs[reader]->parse(encoded);codecs[reader]->read_back(got);require(got==representation,"cross JSON roundtrip mismatch");cross++;}
      }
    }
    for(size_t k=0;k<4;k++){
      auto c=factories[k](shape);Record source=input[3],original=source;
      if(binary&&k!=0){transform(source,original,shape,true);source=original;}
      c->load(source);for(auto&s:source.parts)s.assign(s.size(),'X');std::string encoded(c->encode());c->parse(encoded);c->read_back(got);require(got==original,"encoder borrowed input");ownership++;
      std::fill(encoded.begin(),encoded.end(),'X');c->read_back(got);require(got==original,"decoder borrowed input");ownership++;
    }
  }
  std::vector<std::string>bad_json={"","{","{} garbage","{} {}","[]","null","{\"x\":[1,]}","{\"x\":{\"a\":}}","{\"x\":\"\\uD800\"}",std::string("{\"x\":\"")+char(0xff)+"\"}","{\"x\":01}","{/*comment*/}","{\"x\":NaN}","{\"x\":Infinity}"};
  for(Shape shape:{Shape::Flat,Shape::Nested})for(size_t k=1;k<4;k++){
    auto c=factories[k](shape);Record sample=make_records(shape,64,false)[0];auto full=json_record(sample,shape);
    for(auto bad:bad_json){bool fail=false;try{c->parse(bad);c->read_back(got);}catch(...){fail=true;}require(fail,"malformed JSON accepted");negative++;}
    for(auto value:{nlohmann::ordered_json(-1),nlohmann::ordered_json("1"),nlohmann::ordered_json(true),nlohmann::ordered_json(1.5)}){
      auto altered=full;auto&target=shape==Shape::Flat?altered:altered["common"];target["id"]=value;bool fail=false;try{c->parse(altered.dump());c->read_back(got);}catch(...){fail=true;}require(fail,"invalid unsigned integer accepted");negative++;
    }
    auto overflow=full;auto&target=shape==Shape::Flat?overflow:overflow["common"];target["producer"]=UINT64_C(4294967296);bool fail=false;try{c->parse(overflow.dump());c->read_back(got);}catch(...){fail=true;}require(fail,"u32 overflow accepted");negative++;
    std::string overflow64=full.dump();auto marker=overflow64.find(std::to_string(sample.h.id));require(marker!=std::string::npos,"id marker not found");overflow64.replace(marker,std::to_string(sample.h.id).size(),"18446744073709551616");fail=false;try{c->parse(overflow64);c->read_back(got);}catch(...){fail=true;}require(fail,"u64 overflow accepted");negative++;
    auto unknown=full;unknown["unknown"]=nlohmann::ordered_json{{"nested",{1,true,nullptr,"ok"}}};c->parse(unknown.dump()+" \n\t");c->read_back(got);require(got==sample,"valid unknown field failed");
    // Sorting changes the declaration field order; adapters must not require it.
    nlohmann::json sorted=full;c->parse(sorted.dump());c->read_back(got);require(got==sample,"reordered fields failed");
  }
  std::filesystem::create_directories("results");
  nlohmann::ordered_json result={{"status","PASS"},{"own_roundtrips",records},{"cross_library_json_roundtrips",cross},{"ownership_checks",ownership},{"rejected_inputs",negative},{"base64_cases",b64},{"full_field_and_payload_comparison",true},{"note","Negative JSON schema/type checks include read_back; DOM parse_only is syntax/UTF-8 parsing, not schema validation. Producer records contain every declared field and no duplicate keys."}};
  std::ofstream("results/functional_validation.json")<<result.dump(2)<<'\n';std::cout<<result.dump()<<'\n';
}
int main(int argc,char**argv){try{
  bool check=false;int repeats=5;uint64_t min_ns=80000000;std::string path="results/benchmark.csv";
  for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--smoke")check=true;else if(a=="--output"&&i+1<argc)path=argv[++i];else if(a=="--repeats"&&i+1<argc)repeats=std::stoi(argv[++i]);else throw std::runtime_error("invalid argument");}
  if(check){smoke();return 0;}require(repeats>=1,"invalid repeats");
  cpu_set_t affinity;CPU_ZERO(&affinity);CPU_SET(0,&affinity);require(sched_setaffinity(0,sizeof(affinity),&affinity)==0,"CPU0 affinity failed");
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());std::ofstream f(path);require(bool(f),"cannot open output");f<<"library,schema,profile,tail_bytes,stage,repeat,iterations,wall_ns,cpu_ns,wall_ns_per_event,cpu_ns_per_event,encoded_bytes_mean,decode_input_bytes_mean,checksum,order_index\n"<<std::setprecision(12);
  std::vector<Config>configs;for(auto shape:{Shape::Flat,Shape::Nested})for(size_t tail:{0,64,256,1024})for(bool binary:{false,true})for(size_t lib=0;lib<4;lib++)configs.push_back({shape,tail,binary,lib});
  std::mt19937 rng(20261029);size_t order=0;
  for(int rep=0;rep<repeats;rep++){
    std::shuffle(configs.begin(),configs.end(),rng);
    for(auto cfg:configs){Context context(cfg);std::vector<Stage>stages={Stage::Prepared,Stage::Fields,Stage::Parse,Stage::Decode};if(cfg.binary){stages.push_back(Stage::RawEncode);stages.push_back(Stage::RawDecode);}std::shuffle(stages.begin(),stages.end(),rng);
      for(auto stage:stages){auto m=measure(context,stage,min_ns);f<<context.codec->name()<<','<<shape_name(cfg.shape)<<','<<(cfg.binary?"binary_base64":"utf8_escaped")<<','<<cfg.tail<<','<<stage_name(stage)<<','<<rep<<','<<m.n<<','<<m.wall<<','<<m.cpu<<','<<double(m.wall)/m.n<<','<<double(m.cpu)/m.n<<','<<context.encoded_bytes<<','<<context.decode_bytes<<','<<m.checksum<<','<<order++<<'\n';f.flush();}
    }std::cerr<<"repeat="<<rep<<" rows="<<order<<'\n';
  }std::cout<<"PASS rows="<<order<<"\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
