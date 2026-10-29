#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
namespace jsonbench {
enum class Shape { Flat, Nested };
struct Scalars {
  uint64_t id{}, seq{}, timestamp{}, value0{}, value1{};
  uint32_t producer{}, stream{}, type{}, flags{};
  bool operator==(const Scalars&) const = default;
};
struct Record {
  Scalars h;
  // Flat uses parts[0] only; Nested uses all three. String values supplied to
  // JSON adapters are already UTF-8 text or canonical padded Base64.
  std::array<std::string,3> parts;
  bool operator==(const Record&) const = default;
};
class Codec {
public:
  virtual ~Codec() = default;
  virtual const char* name() const = 0;
  // Copy every field into a reusable owning encoder object/DOM; no borrowed
  // application string lifetime. Preserve uint64_t exactly, never via double.
  virtual void load(const Record&) = 0;
  // Return a view valid until the next load/encode or object destruction.
  virtual std::string_view encode() = 0;
  // Strict standard JSON, complete document, valid UTF-8, owning decoded values.
  // PB implementation similarly retains an owning generated message.
  virtual void parse(std::string_view) = 0;
  // Read all scalars and copy every string into an owning canonical record.
  // Separate from parse so DOM construction isn't mistaken for field access.
  virtual void read_back(Record&) const = 0;
  virtual const void* parsed_state() const = 0;
};
std::unique_ptr<Codec> make_nlohmann(Shape);
std::unique_ptr<Codec> make_yyjson(Shape);
std::unique_ptr<Codec> make_glaze(Shape);
std::unique_ptr<Codec> make_protobuf(Shape);
} // namespace jsonbench
