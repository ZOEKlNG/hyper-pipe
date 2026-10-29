#include "storage.hpp"
#include "slots.hpp"
#include <iostream>
using namespace sf;
int main(){
 Corpus corpus(64,3);StorageManager store(Storage::M2);Session session(0,3,false);
 store.prepare(session,7);store.update(session,0,corpus);store.update(session,1,corpus);
 const auto&base=static_cast<const Export&>(session.modules[2]);Export shallow=base;
 std::string deep(shallow.text[0].str());char before=deep.front();
 session.owned[2*STRINGS][0]^=0x7f;
 require(shallow.text[0].str()!=deep&&deep.front()==before,"view is not bytes snapshot demo");
 std::cout<<"{\"H5_view_copy_does_not_freeze_bytes\":true,\"deep_copy_preserves_bytes\":true,\"copy_descriptor_bytes\":"<<sizeof(Descriptor)<<",\"pointer_descriptor_bytes\":"<<sizeof(PointerDescriptor)<<",\"export_bytes\":"<<sizeof(Export)<<",\"module_state_bytes\":"<<sizeof(ModuleState)<<",\"string_object_bytes\":"<<sizeof(std::string)<<",\"default_string_capacity\":"<<std::string().capacity()<<",\"base_adjustment_observed\":"<<(reinterpret_cast<const char*>(&base)-reinterpret_cast<const char*>(&session.modules[2]))<<",\"base_offset_is_not_a_portable_contract\":true}\n";
}
