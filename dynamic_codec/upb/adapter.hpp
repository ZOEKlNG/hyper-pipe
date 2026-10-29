#pragma once
#include "../common.hpp"

namespace dynbench {
// Required harness factory. Per-operation message and fixed-size Arena initialization/free.
// Encode borrows payload until synchronous encoding completes and copies final
// wire bytes to the caller-owned std::string. alias affects decode ownership only.
std::unique_ptr<Codec> make_upb_codec(bool fixed,bool alias);

// Ownership/allocation controls for separately labelled sensitivity comparisons.
struct UpbOptions {
  bool fixed=false;
  bool alias_decode=false;
  bool copy_encode_payload=false;
  bool heap_arena=false;
};
std::unique_ptr<Codec> make_upb_codec_ex(UpbOptions options);

// Functional checks only; not part of the benchmark's hot path.
// Proves alias versus copied payload addresses and copied payload independence.
void upb_adapter_ownership_smoke();
void upb_adapter_oom_smoke();
}
