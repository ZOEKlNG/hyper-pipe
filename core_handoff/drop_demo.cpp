// Paced drop-new queue experiment; not a Worker service-time benchmark.
#define main original_handoff_main
#include "handoff.cpp"
#undef main
struct DropStats{uint64_t offered,accepted,dropped,received,errors,max_queue,max_source_lateness_ns,pause_observed_ns;double send_s,drain_s;};
DropStats drop_once(Mode mode,size_t capacity,unsigned pause_ms,const std::vector<Packet>&corpus){
 constexpr uint64_t N=50000,interval_ns=4000; // 250k offered/s, 200ms planned input.
 Queue q;std::vector<Slot>slots(capacity);if(mode==Mode::A)for(auto&s:slots)new(s.storage.data())NativeSlot;
 std::vector<std::unique_ptr<PBState>>objects;if(mode==Mode::D_pb_object)for(size_t i=0;i<capacity;i++){objects.push_back(std::make_unique<PBState>());fill_pb(*objects.back(),Shape::Nested,extract(corpus[i&(CORPUS_N-1)]));}
 PBState worker;fill_pb(worker,Shape::Nested,extract(corpus.back()));
 std::atomic<bool>done{false};DropStats out{};out.offered=N;uint64_t received=0,errors=0,pause_observed=0,end_rx=0;
 std::thread logger([&]{pin(1);PBState pb;fill_pb(pb,Shape::Nested,extract(corpus.back()));std::array<uint8_t,MAX_WIRE>wire;
  q.ready.v.store(1,std::memory_order_release);while(!q.go.v.load(std::memory_order_acquire))_mm_pause();
  if(pause_ms){auto start=ns();std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));pause_observed=ns()-start;}
  uint64_t tail=0;
  for(;;){uint64_t head=q.head.v.load(std::memory_order_acquire);if(tail==head){if(done.load(std::memory_order_acquire)&&tail==q.head.v.load(std::memory_order_acquire))break;_mm_pause();continue;}
   auto&s=slots[tail&(capacity-1)];if(s.sequence!=tail)++errors;size_t n;
   if(mode==Mode::A)n=encode_pb(pb,Shape::Nested,view_slot(s.native()),wire.data());
   else if(mode==Mode::D_pb_object)n=serialize_pb(*objects[tail&(capacity-1)],Shape::Nested,wire.data(),MAX_WIRE);
   else{n=s.wire_len;std::memcpy(wire.data(),s.storage.data(),n);}
   if(n>MAX_WIRE)++errors;auto witness=wire_checksum(wire.data(),n);escape(&witness);++received;++tail;q.tail.v.store(tail,std::memory_order_release);
  }end_rx=ns();
 });
 pin(0);while(!q.ready.v.load(std::memory_order_acquire))_mm_pause();auto start=ns();q.go.v.store(1,std::memory_order_release);uint64_t head=0;
 for(uint64_t i=0;i<N;i++){
  uint64_t target=start+i*interval_ns,actual;while((actual=ns())<target)_mm_pause();out.max_source_lateness_ns=std::max(out.max_source_lateness_ns,actual-target);
  uint64_t tail=q.tail.v.load(std::memory_order_acquire);if(head-tail>=capacity){++out.dropped;continue;}
  auto&s=slots[head&(capacity-1)];auto v=extract(corpus[i&(CORPUS_N-1)]);
  if(mode==Mode::A)fill_slot(s.native(),v);else if(mode==Mode::D_pb_object)fill_pb(*objects[head&(capacity-1)],Shape::Nested,v);else s.wire_len=uint32_t(encode_pb(worker,Shape::Nested,v,s.storage.data()));
  s.sequence=head;s.corpus_index=i&(CORPUS_N-1);++head;++out.accepted;q.head.v.store(head,std::memory_order_release);out.max_queue=std::max(out.max_queue,head-tail);
 }
 auto send_end=ns();done.store(true,std::memory_order_release);logger.join();out.received=received;out.errors=errors;out.pause_observed_ns=pause_observed;out.send_s=double(send_end-start)/1e9;out.drain_s=double(end_rx-start)/1e9;
 if(out.offered!=out.accepted+out.dropped||out.accepted!=out.received||out.max_queue>capacity||errors)throw std::runtime_error("drop-new conservation failure");return out;
}
int main(){try{
 auto corpus=make_corpus(256);std::ofstream f("results/drop_demo.csv");f<<"mode,queue_slots,logger_pause_requested_ms,repeat,offered,accepted,dropped,received,max_queue,max_source_lateness_ns,pause_observed_ns,send_seconds,drain_seconds\n"<<std::setprecision(12);
 for(unsigned pause:{0,10})for(size_t cap:{256,4096})for(Mode mode:{Mode::A,Mode::B,Mode::D_pb_object})for(int rep=0;rep<3;rep++){
  auto r=drop_once(mode,cap,pause,corpus);f<<mode_name(mode)<<','<<cap<<','<<pause<<','<<rep<<','<<r.offered<<','<<r.accepted<<','<<r.dropped<<','<<r.received<<','<<r.max_queue<<','<<r.max_source_lateness_ns<<','<<r.pause_observed_ns<<','<<r.send_s<<','<<r.drain_s<<'\n';f.flush();
 }return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
