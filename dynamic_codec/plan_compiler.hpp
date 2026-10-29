#pragma once
#include "common.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <unordered_set>
namespace dynbench {
struct FieldMeta {const char*name;uint32_t fid;ScalarKind kind;size_t offset;const char*type;};
inline const std::array<FieldMeta,10> registry={{
 {"id",1,ScalarKind::U64,offsetof(Header,id),"u64"},
 {"seq",2,ScalarKind::U64,offsetof(Header,seq),"u64"},
 {"timestamp",3,ScalarKind::U64,offsetof(Header,timestamp),"u64"},
 {"value0",4,ScalarKind::U64,offsetof(Header,value0),"u64"},
 {"value1",5,ScalarKind::U64,offsetof(Header,value1),"u64"},
 {"producer",6,ScalarKind::U32,offsetof(Header,producer),"u32"},
 {"stream",7,ScalarKind::U32,offsetof(Header,stream),"u32"},
 {"type",8,ScalarKind::U32,offsetof(Header,type),"u32"},
 {"flags",9,ScalarKind::U32,offsetof(Header,flags),"u32"},
 {"payload",10,ScalarKind::Bytes,0,"bytes"}
}};
inline std::string varint_bytes(uint64_t x){std::string s;while(x>=128){s.push_back(char((x&127)|128));x>>=7;}s.push_back(char(x));return s;}
inline void only_keys(const nlohmann::json&j,std::initializer_list<const char*>keys){
 if(!j.is_object())throw std::invalid_argument("configuration must be object");
 for(auto it=j.begin();it!=j.end();++it){bool ok=false;for(auto k:keys)if(it.key()==k)ok=true;if(!ok)throw std::invalid_argument("unsupported configuration key: "+it.key());}
}
inline Plan compile_plan(const nlohmann::json&j){
 only_keys(j,{"name","fields"});
 if(!j.contains("name")||!j["name"].is_string()||j["name"].get<std::string>().empty())throw std::invalid_argument("plan requires nonempty name");
 if(!j.contains("fields")||!j["fields"].is_array())throw std::invalid_argument("plan requires fields array");
 Plan p;p.name=j["name"].get<std::string>();
 for(const auto&entry:j["fields"]){
  std::string name;
  if(entry.is_string())name=entry.get<std::string>();
  else{only_keys(entry,{"name","fid","type"});if(!entry.contains("name")||!entry["name"].is_string())throw std::invalid_argument("field requires name");name=entry["name"].get<std::string>();}
  const FieldMeta* meta=nullptr;for(const auto&m:registry)if(name==m.name)meta=&m;
  if(!meta)throw std::invalid_argument("unknown field: "+name);
  if(!entry.is_string()){
   if(entry.contains("fid")&&(!entry["fid"].is_number_unsigned()&&!entry["fid"].is_number_integer()))throw std::invalid_argument("fid must be integer");
   if(entry.contains("fid")&&entry["fid"].get<int64_t>()!=meta->fid)throw std::invalid_argument("fid does not match registry");
   if(entry.contains("type")&&(!entry["type"].is_string()||entry["type"].get<std::string>()!=meta->type))throw std::invalid_argument("type does not match registry");
  }
  if(p.has(meta->fid))throw std::invalid_argument("duplicate fid: "+std::to_string(meta->fid));
  FieldOp op;op.fid=meta->fid;op.kind=meta->kind;op.source_offset=meta->offset;op.is_payload=meta->kind==ScalarKind::Bytes;
  op.tag_bytes=varint_bytes((op.fid<<3)|(op.is_payload?2:0));
  op.fixed_tag_bytes=varint_bytes((op.fid<<3)|(op.is_payload?2:op.kind==ScalarKind::U64?1:5));
  p.ops.push_back(std::move(op));p.mask|=uint16_t(1)<<(meta->fid-1);
 }
 return p;
}
inline std::vector<Plan> compile_config(std::string_view text){
 auto j=nlohmann::json::parse(text);only_keys(j,{"version","profiles"});
 if(!j.contains("version")||j["version"]!=1)throw std::invalid_argument("unsupported config version");
 if(!j.contains("profiles")||!j["profiles"].is_array()||j["profiles"].empty())throw std::invalid_argument("profiles array required");
 std::vector<Plan> plans;std::unordered_set<std::string> names;
 for(const auto&row:j["profiles"]){auto p=compile_plan(row);if(!names.insert(p.name).second)throw std::invalid_argument("duplicate plan name");plans.push_back(std::move(p));}
 return plans;
}
inline size_t plan_index(const std::vector<Plan>&plans,std::string_view name){for(size_t i=0;i<plans.size();i++)if(plans[i].name==name)return i;throw std::invalid_argument("missing required profile "+std::string(name));}
}
