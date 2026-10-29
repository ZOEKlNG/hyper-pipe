// Supplement: exact-size, validated-plan Protobuf emitter. Original benchmark is unchanged.
#define main original_benchmark_main
#include "bench.cpp"
#undef main

class ExactSizeEmit final:public Codec {
 bool fixed_;
public:
 explicit ExactSizeEmit(bool f):fixed_(f){}
 const char* name()const override{return fixed_?"custom_proto_emit_exactsize_fixed":"custom_proto_emit_exactsize_varint";}
 __attribute__((noinline)) size_t encode(const Event&e,const Plan&p,std::string&out)override{
  static_assert(std::endian::native==std::endian::little,"supplement is LE-only");
  // The trusted registry/Plan has been checked before timing; max ten fields.
  uint64_t values[10]{};size_t n=0,k=0;
  for(const auto&op:p.ops){
   uint64_t v=op.is_payload?e.payload.size():scalar_value(e,op);values[k++]=v;
   if(v==0)continue;
   const auto&tag=fixed_?op.fixed_tag_bytes:op.tag_bytes;n+=tag.size();
   if(op.is_payload)n+=v+(std::bit_width(v)+6)/7;
   else n+=fixed_?(op.kind==ScalarKind::U32?4:8):(std::bit_width(v)+6)/7;
  }
  out.resize(n);char*q=out.data();k=0;
  for(const auto&op:p.ops){
   uint64_t v=values[k++];if(v==0)continue;
   const auto&tag=fixed_?op.fixed_tag_bytes:op.tag_bytes;memcpy(q,tag.data(),tag.size());q+=tag.size();
   if(op.is_payload){custom_detail::write_varint(q,v);memcpy(q,e.payload.data(),v);q+=v;}
   else if(fixed_){if(op.kind==ScalarKind::U32){uint32_t w=uint32_t(v);memcpy(q,&w,4);q+=4;}else{memcpy(q,&v,8);q+=8;}}
   else custom_detail::write_varint(q,v);
  }
  if(size_t(q-out.data())!=n)throw std::runtime_error("exact-size emitter length mismatch");
  escape(out.data());return n;
 }
 uint64_t decode(std::string_view w,bool few)override{return pb_wire_view_checked(w,fixed_?PBScalarWire::Fixed:PBScalarWire::Varint).checksum(few);}
};
int main(){try{
 cpu_set_t set;CPU_ZERO(&set);CPU_SET(0,&set);if(sched_setaffinity(0,sizeof(set),&set))throw std::runtime_error("pin CPU0 failed");
 Plans plans;plans.values=compile_config(read_file("plans.json"));plans.full=plan_index(plans.values,"full");plans.subset=plan_index(plans.values,"subset");plans.no_payload=plan_index(plans.values,"no_payload");
 for(const auto&p:plans.values)validate_custom_plan(p);
 std::vector<std::unique_ptr<Codec>> codecs;
 codecs.emplace_back(new GeneratedPB<bench::FixedEvent,false>);codecs.emplace_back(new ProtoEmitCodec(true));codecs.emplace_back(new ExactSizeEmit(true));
 codecs.emplace_back(new GeneratedPB<bench::Event,false>);codecs.emplace_back(new ProtoEmitCodec(false));codecs.emplace_back(new ExactSizeEmit(false));
 auto events=make_events(256,"random");
 struct Case{Codec*c;std::string profile;std::array<const Plan*,CORPUS_N>refs;std::string out;};std::vector<Case>cases;
 uint64_t validated=0;
 for(auto&c:codecs)for(std::string prof:{"full","subset","mixed"}){
  Case x{c.get(),prof,{},{}};
  for(size_t i=0;i<CORPUS_N;i++){
   x.refs[i]=&plans.get(prof,i);std::string w;c->encode(events[i],*x.refs[i],w);validate_wire(*c,w,events[i],*x.refs[i]);
   bool f=std::string(c->name()).find("fixed")!=std::string::npos;ProtoEmitCodec original(f);std::string ref;original.encode(events[i],*x.refs[i],ref);if(w!=ref)throw std::runtime_error("byte mismatch with checked reference emitter");++validated;
  }
  x.out.reserve(512);cases.push_back(std::move(x));
 }
 std::ofstream f("results/optimized_encode.csv");f<<"encoder,profile,payload_bytes,operation,repeat,iterations,elapsed_ns,cpu_ns,ns_per_op,checksum\n"<<std::setprecision(12);
 std::vector<size_t> order(cases.size());std::iota(order.begin(),order.end(),0);std::mt19937 gen(20261029);
 for(int rep=0;rep<5;rep++){std::shuffle(order.begin(),order.end(),gen);for(auto j:order){auto&x=cases[j];auto m=measure([&](size_t i){x.c->encode(events[i],*x.refs[i],x.out);return x.out.size()+(x.out.empty()?0:uint8_t(x.out.front())+uint8_t(x.out.back()));},100000000);f<<x.c->name()<<','<<x.profile<<",256,encode,"<<rep<<','<<m.iterations<<','<<m.elapsed_ns<<','<<m.cpu_ns<<','<<m.ns<<','<<m.checksum<<'\n';f.flush();}}
 std::ofstream v("results/optimized_validation.json");v<<nlohmann::json({{"semantic_and_byte_identical_cases",validated},{"seed",20261029},{"trusted_plan_boundary","registry and field operations validated before timing; no generic-schema claim"},{"output_boundary","owning std::string, reused capacity, exact-size computation; includes source reads and payload copy"}}).dump(2)<<'\n';
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
