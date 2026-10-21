// Linux x86_64 SPSC bounded shared pool benchmark, no external dependencies.
// Atomic index publication transfers ownership. Descriptors use offsets, never wire pointers.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <new>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>

static_assert(std::atomic<uint64_t>::is_always_lock_free);
constexpr uint64_t CAP=4096;
struct alignas(128) Counter {std::atomic<uint64_t> value{0};};
struct Descriptor {uint64_t offset;uint64_t seq;uint32_t length;uint32_t reserved;};
struct alignas(128) Shared {
    Counter head,tail,ready,start,done;
    alignas(128) uint64_t checksum=0,errors=0;
    double consumer_cpu_s=0;
};
uint64_t ns(clockid_t clock=CLOCK_MONOTONIC) {timespec t;clock_gettime(clock,&t);return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;}
void pin(int cpu){cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof(set),&set)!=0){std::cerr<<"affinity "<<cpu<<": "<<strerror(errno)<<"\n";std::exit(2);}}
inline void relax(){_mm_pause();}
__attribute__((noinline)) void construct(uint64_t* p,size_t words,uint64_t seq){for(size_t j=0;j<words;j++)p[j]=seq+j;}
__attribute__((noinline)) uint64_t consume(const uint64_t* p,size_t words){uint64_t x=0;for(size_t j=0;j<words;j++)x+=p[j];return x;}
struct Result{double elapsed_s,producer_cpu_s,consumer_cpu_s;uint64_t checksum,errors;};
Result run_once(bool processes,bool copy_mode,size_t bytes,uint64_t batch,uint64_t messages){
    size_t stride=(bytes+63)&~size_t(63);
    size_t desc_offset=sizeof(Shared),pool_offset=(desc_offset+CAP*sizeof(Descriptor)+63)&~size_t(63);
    size_t mapping_size=pool_offset+CAP*stride;
    auto* base=static_cast<char*>(mmap(nullptr,mapping_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
    if(base==MAP_FAILED){perror("mmap");std::exit(2);}
    auto* s=new(base) Shared;
    auto* desc=reinterpret_cast<Descriptor*>(base+desc_offset);
    // Fault pages outside timing. Slots are all producer-owned before head publication.
    memset(base+desc_offset,0,mapping_size-desc_offset);
    auto receiver=[&](){
        pin(1);s->ready.value.store(1,std::memory_order_release);
        while(!s->start.value.load(std::memory_order_acquire))relax();
        uint64_t cpu0=ns(CLOCK_THREAD_CPUTIME_ID),tail=0,checksum=0,errors=0;
        while(tail<messages){
            uint64_t head=s->head.value.load(std::memory_order_acquire);
            if(head==tail){relax();continue;}
            uint64_t take=std::min(batch,head-tail);
            for(uint64_t j=0;j<take;j++){
                auto& d=desc[(tail+j)&(CAP-1)];
                uint64_t seq=tail+j;size_t words=bytes/8;
                if(d.offset!=pool_offset+((seq&(CAP-1))*stride)||d.length!=bytes||d.seq!=seq){++errors;continue;}
                uint64_t value=consume(reinterpret_cast<const uint64_t*>(base+d.offset),words);
                uint64_t expected=words*seq+(words*(words-1))/2;
                if(value!=expected)++errors;
                checksum+=value;
            }
            tail+=take;s->tail.value.store(tail,std::memory_order_release);
        }
        s->checksum=checksum;s->errors=errors;s->consumer_cpu_s=(ns(CLOCK_THREAD_CPUTIME_ID)-cpu0)/1e9;
        s->done.value.store(1,std::memory_order_release);
    };
    std::thread thread;pid_t child=-1;
    if(processes){child=fork();if(child<0){perror("fork");std::exit(2);}if(child==0){receiver();_exit(0);}}
    else thread=std::thread(receiver);
    pin(0);
    while(!s->ready.value.load(std::memory_order_acquire))relax();
    std::vector<uint64_t> scratch(bytes/8);
    uint64_t head=0,wall0=ns(),cpu0=ns(CLOCK_THREAD_CPUTIME_ID);
    s->start.value.store(1,std::memory_order_release);
    while(head<messages){
        uint64_t take=std::min(batch,messages-head);
        uint64_t tail=s->tail.value.load(std::memory_order_acquire);
        if(head+take-tail>CAP){relax();continue;}
        for(uint64_t j=0;j<take;j++){
            uint64_t seq=head+j,offset=pool_offset+((seq&(CAP-1))*stride);
            auto* slot=reinterpret_cast<uint64_t*>(base+offset);
            if(copy_mode){construct(scratch.data(),bytes/8,seq);memcpy(slot,scratch.data(),bytes);}
            else construct(slot,bytes/8,seq);
            desc[seq&(CAP-1)]={offset,seq,static_cast<uint32_t>(bytes),0};
        }
        head+=take;s->head.value.store(head,std::memory_order_release);
    }
    while(!s->done.value.load(std::memory_order_acquire))relax();
    uint64_t wall1=ns(),cpu1=ns(CLOCK_THREAD_CPUTIME_ID);
    Result result{(wall1-wall0)/1e9,(cpu1-cpu0)/1e9,s->consumer_cpu_s,s->checksum,s->errors};
    if(processes){int status=0;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::cerr<<"child failure\n";std::exit(2);}}
    else thread.join();
    uint64_t words=bytes/8;
    uint64_t expected=words*(messages*(messages-1)/2)+messages*(words*(words-1)/2);
    if(result.checksum!=expected||result.errors){std::cerr<<"verification failed\n";std::exit(3);}
    s->~Shared();munmap(base,mapping_size);return result;
}
int main(int argc,char**argv){
    std::string out=argc>1?argv[1]:"ipc_results.csv";
    uint64_t requested_messages=argc>2?std::strtoull(argv[2],nullptr,10):0;
    double target_seconds=argc>4?std::atof(argv[4]):0.35;
    int repeats=argc>3?std::atoi(argv[3]):5;
    std::string filter=argc>5?argv[5]:"";
    alarm(300);
    std::ofstream f(out);
    f<<"model,construction,message_bytes,batch,capacity,messages,repeat,elapsed_s,msg_per_s,ns_per_delivered,producer_cpu_s,consumer_cpu_s,total_cpu_ns_per_msg,dropped,checksum,errors\n";
    for(bool processes:{false,true})for(bool copy_mode:{false,true})for(size_t bytes:{64,256,1024})for(uint64_t batch:{1,32}){
        std::string key=std::string(processes?"fork_mmap":"threads")+","+(copy_mode?"scratch_then_copy":"inplace_pool")+","+std::to_string(bytes)+","+std::to_string(batch);
        if(!filter.empty() && filter!=key)continue;
        auto calibration=run_once(processes,copy_mode,bytes,batch,200000);
        uint64_t messages=requested_messages;
        if(!messages){
            messages=static_cast<uint64_t>(std::ceil(200000*target_seconds/calibration.elapsed_s/32))*32;
            messages=std::clamp(messages,uint64_t(100000),uint64_t(100000000));
        }
        for(int rep=0;rep<repeats;rep++){
            auto r=run_once(processes,copy_mode,bytes,batch,messages);
            if(!requested_messages){
                while(r.elapsed_s<target_seconds*0.85 && messages<100000000){
                    messages=std::min(uint64_t(100000000),static_cast<uint64_t>(std::ceil(messages*target_seconds/r.elapsed_s/32))*32);
                    r=run_once(processes,copy_mode,bytes,batch,messages);
                }
            }
            f.precision(12);f<<(processes?"fork_mmap":"threads")<<','<<(copy_mode?"scratch_then_copy":"inplace_pool")<<','<<bytes<<','<<batch<<','<<CAP<<','<<messages<<','<<rep<<','<<r.elapsed_s<<','<<messages/r.elapsed_s<<','<<r.elapsed_s*1e9/messages<<','<<r.producer_cpu_s<<','<<r.consumer_cpu_s<<','<<(r.producer_cpu_s+r.consumer_cpu_s)*1e9/messages<<",0,"<<r.checksum<<','<<r.errors<<'\n';
            f.flush();
        }
        std::cerr<<(processes?"process":"thread")<<' '<<(copy_mode?"copy":"inplace")<<' '<<bytes<<' '<<batch<<" complete\n";
    }
}
