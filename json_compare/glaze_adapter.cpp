#include "adapter.hpp"
#include <glaze/json.hpp>
#include <stdexcept>
#include <utility>

namespace jsonbench::glaze_detail {
struct Flat {
  uint64_t id{},seq{},timestamp{},value0{},value1{};
  uint32_t producer{},stream{},type{},flags{};
  std::string payload;
};
struct Common {
  uint64_t id{},seq{},timestamp{},value0{},value1{};
  uint32_t producer{},stream{};
};
struct Http {
  uint32_t status{},method{};
  std::string url,host,cookie;
};
struct Nested {Common common;Http http;};
struct StrictOptions:glz::opts {
  bool validate_utf8=true;
  bool validate_skipped=true;
  bool validate_trailing_whitespace=true;
  bool escape_control_characters=true;
};
inline constexpr auto strict_options=[] {
  StrictOptions o;
  o.null_terminated=false;
  o.error_on_unknown_keys=false;
  o.error_on_missing_keys=false;
  o.comments=false;
  o.minified=false;
  o.partial_read=false;
  o.internal=0; // No is_padded assertion for arbitrary caller-owned string_view.
  return o;
}();
static_assert(!strict_options.null_terminated);
static_assert(strict_options.validate_utf8 && strict_options.validate_skipped && strict_options.validate_trailing_whitespace);
static_assert(!glz::check_is_padded(strict_options));
inline void reset(Flat&v) {
  v.id=v.seq=v.timestamp=v.value0=v.value1=0;
  v.producer=v.stream=v.type=v.flags=0;
  v.payload.clear(); // Retain reusable capacity; absent members receive defaults.
}
inline void reset(Nested&v) {
  v.common={};v.http.status=v.http.method=0;
  v.http.url.clear();v.http.host.clear();v.http.cookie.clear();
}
}

namespace glz {
template<>struct meta<jsonbench::glaze_detail::Flat> {
  using T=jsonbench::glaze_detail::Flat;
  static constexpr auto value=object("id",&T::id,"seq",&T::seq,"timestamp",&T::timestamp,"value0",&T::value0,"value1",&T::value1,
    "producer",&T::producer,"stream",&T::stream,"type",&T::type,"flags",&T::flags,"payload",&T::payload);
};
template<>struct meta<jsonbench::glaze_detail::Common> {
  using T=jsonbench::glaze_detail::Common;
  static constexpr auto value=object("id",&T::id,"seq",&T::seq,"timestamp",&T::timestamp,"value0",&T::value0,"value1",&T::value1,"producer",&T::producer,"stream",&T::stream);
};
template<>struct meta<jsonbench::glaze_detail::Http> {
  using T=jsonbench::glaze_detail::Http;
  static constexpr auto value=object("status",&T::status,"method",&T::method,"url",&T::url,"host",&T::host,"cookie",&T::cookie);
};
template<>struct meta<jsonbench::glaze_detail::Nested> {
  using T=jsonbench::glaze_detail::Nested;
  static constexpr auto value=object("common",&T::common,"http",&T::http);
};
}

namespace jsonbench {
namespace {
class GlazeCodec final:public Codec {
  Shape shape_;
  glaze_detail::Flat flat_encoder_,flat_decoder_;
  glaze_detail::Nested nested_encoder_,nested_decoder_;
  std::string output_;
public:
  explicit GlazeCodec(Shape shape):shape_(shape) {output_.reserve(2048);}
  const char*name()const override{return "glaze_9_1_0_typed_strict_owning_reuse";}
  void load(const Record&r)override {
    const auto&h=r.h;
    if(shape_==Shape::Flat) {
      auto&v=flat_encoder_;v.id=h.id;v.seq=h.seq;v.timestamp=h.timestamp;v.value0=h.value0;v.value1=h.value1;
      v.producer=h.producer;v.stream=h.stream;v.type=h.type;v.flags=h.flags;v.payload.assign(r.parts[0]);
    } else {
      auto&c=nested_encoder_.common;auto&v=nested_encoder_.http;
      c.id=h.id;c.seq=h.seq;c.timestamp=h.timestamp;c.value0=h.value0;c.value1=h.value1;c.producer=h.producer;c.stream=h.stream;
      v.status=h.type;v.method=h.flags;v.url.assign(r.parts[0]);v.host.assign(r.parts[1]);v.cookie.assign(r.parts[2]);
    }
  }
  std::string_view encode()override {
    const auto ec=shape_==Shape::Flat?glz::write<glaze_detail::strict_options>(flat_encoder_,output_):glz::write<glaze_detail::strict_options>(nested_encoder_,output_);
    if(ec)throw std::runtime_error("Glaze JSON encode failed, error="+std::to_string(static_cast<unsigned>(ec.ec)));
    return output_;
  }
  void parse(std::string_view wire)override {
    // Do not let absent members retain values from a prior message. String clear
    // keeps allocations available for Glaze's owning std::string decode.
    if(shape_==Shape::Flat) {
      glaze_detail::reset(flat_decoder_);
      const auto ec=glz::read<glaze_detail::strict_options>(flat_decoder_,wire);
      if(ec)throw std::runtime_error("Glaze JSON parse failed, error="+std::to_string(static_cast<unsigned>(ec.ec)));
    } else {
      glaze_detail::reset(nested_decoder_);
      const auto ec=glz::read<glaze_detail::strict_options>(nested_decoder_,wire);
      if(ec)throw std::runtime_error("Glaze JSON parse failed, error="+std::to_string(static_cast<unsigned>(ec.ec)));
    }
  }
  void read_back(Record&r)const override {
    if(shape_==Shape::Flat) {
      const auto&v=flat_decoder_;
      r.h={v.id,v.seq,v.timestamp,v.value0,v.value1,v.producer,v.stream,v.type,v.flags};
      r.parts[0].assign(v.payload);r.parts[1].clear();r.parts[2].clear();
    } else {
      const auto&c=nested_decoder_.common;const auto&v=nested_decoder_.http;
      r.h={c.id,c.seq,c.timestamp,c.value0,c.value1,c.producer,c.stream,v.status,v.method};
      r.parts[0].assign(v.url);r.parts[1].assign(v.host);r.parts[2].assign(v.cookie);
    }
  }
  const void*parsed_state()const override {
    return shape_==Shape::Flat?static_cast<const void*>(&flat_decoder_):static_cast<const void*>(&nested_decoder_);
  }
};
}
std::unique_ptr<Codec>make_glaze(Shape shape){return std::make_unique<GlazeCodec>(shape);}
}
