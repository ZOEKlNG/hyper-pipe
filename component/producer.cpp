#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "wire.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <cerrno>
#include <cmath>
using Clock=std::chrono::steady_clock;
struct Route{sockaddr_in addr{};uint32_t epoch=1;};
static Route route(const std::string&host,int port,uint32_t epoch){Route r;r.addr.sin_family=AF_INET;r.addr.sin_port=htons(port);r.epoch=epoch;if(inet_pton(AF_INET,host.c_str(),&r.addr.sin_addr)!=1)throw std::runtime_error("use IPv4 literal host");return r;}
int main(int argc,char**argv){try{
 std::string host="127.0.0.1",file;int port=19090,batch=32,flush_us=10;size_t n=1000,payload=256,buckets=4096;double rate=0;uint64_t producer=1,session=std::chrono::system_clock::now().time_since_epoch().count();bool malformed=false;
 for(int i=1;i<argc;i++){std::string k=argv[i];auto val=[&](){if(++i>=argc)throw std::runtime_error("missing argument");return std::string(argv[i]);};if(k=="--host")host=val();else if(k=="--port")port=stoi(val());else if(k=="--count")n=stoull(val());else if(k=="--payload")payload=stoull(val());else if(k=="--batch")batch=stoi(val());else if(k=="--flush-us")flush_us=stoi(val());else if(k=="--rate")rate=stod(val());else if(k=="--routes")file=val();else if(k=="--buckets")buckets=stoull(val());else if(k=="--producer")producer=stoull(val());else if(k=="--session")session=stoull(val());else if(k=="--malformed")malformed=true;else throw std::runtime_error("unknown argument: "+k);}
 if(port<1||port>65535||batch<1||batch>1024||flush_us<1||!std::isfinite(rate)||rate<0||buckets==0||(buckets&(buckets-1))||buckets>65536||payload>hp::MAX_DATAGRAM-hp::HEADER)throw std::runtime_error("invalid bounded configuration");
 std::vector<Route>routes(buckets,route(host,port,1));if(!file.empty()){std::ifstream in(file);if(!in)throw std::runtime_error("cannot read route snapshot");std::string line;std::vector<bool>seen(buckets);while(getline(in,line)){if(line.empty()||line[0]=='#'||line.rfind("bucket,",0)==0)continue;std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);size_t b;std::string h;int p;uint32_t epoch;if(!(row>>b>>h>>p>>epoch)||b>=buckets||p<1||p>65535||seen[b])throw std::runtime_error("invalid or duplicate route row");routes[b]=route(h,p,epoch);seen[b]=true;}}
 int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);if(fd<0)throw std::runtime_error("socket failed");
 std::vector<std::array<std::byte,hp::MAX_DATAGRAM>>buffers(batch);std::vector<iovec>iov(batch);std::vector<mmsghdr>msgs(batch);std::vector<uint64_t>seq(buckets);size_t pending=0,accepted=0,dropped=0,calls=0;Clock::time_point deadline;
 auto flush=[&](){if(!pending)return;int result=sendmmsg(fd,msgs.data(),pending,MSG_DONTWAIT);calls++;if(result<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=ENOBUFS)std::cerr<<"sendmmsg errno="<<errno<<"\n";result=0;}accepted+=result;dropped+=pending-result;pending=0;};
 auto start=Clock::now(),next=start;
 for(size_t i=0;i<n;){auto now=Clock::now();if(pending&&now>=deadline)flush();if(rate>0&&now<next){auto wake=pending?std::min(next,deadline):next;std::this_thread::sleep_until(wake);continue;}uint64_t id=i%65536;size_t b=hp::mix64(id)&(buckets-1);auto&r=routes[b];hp::Event e{producer,session,seq[b]++,id,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),uint32_t(b),r.epoch,1};size_t size=hp::encode(buffers[pending].data(),buffers[pending].size(),e,payload);if(malformed&&i%3==0)hp::u16(buffers[pending].data()+4,99);else if(malformed&&i%3==1)hp::u32(buffers[pending].data()+68,payload+1);else if(malformed)hp::u32(buffers[pending].data(),0);iov[pending]={buffers[pending].data(),size};msgs[pending]={};msgs[pending].msg_hdr.msg_name=&r.addr;msgs[pending].msg_hdr.msg_namelen=sizeof(r.addr);msgs[pending].msg_hdr.msg_iov=&iov[pending];msgs[pending].msg_hdr.msg_iovlen=1;if(pending++==0)deadline=now+std::chrono::microseconds(flush_us);if(pending==size_t(batch))flush();i++;if(rate>0)next=start+std::chrono::nanoseconds(uint64_t(double(i)*1e9/rate));}
 flush();close(fd);double sec=std::chrono::duration<double>(Clock::now()-start).count();std::cout<<"{\"offered\":"<<n<<",\"kernel_accepted\":"<<accepted<<",\"local_send_drop\":"<<dropped<<",\"syscalls\":"<<calls<<",\"elapsed_s\":"<<sec<<",\"payload_bytes\":"<<payload<<",\"wire_bytes\":"<<hp::HEADER+payload<<",\"buckets\":"<<buckets<<",\"session\":"<<session<<"}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
