#define _GNU_SOURCE
#include <zmq.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include <string>

using Clock=std::chrono::steady_clock;
static double now(){return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();}
static double cput(clockid_t c){timespec t{};clock_gettime(c,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void pin(int cpu){cpu_set_t s;CPU_ZERO(&s);CPU_SET(cpu,&s);if(pthread_setaffinity_np(pthread_self(),sizeof(s),&s))throw std::runtime_error("cannot pin CPU");}
static void ck(bool good,const char* s){if(!good)throw std::runtime_error(std::string(s)+": "+strerror(errno));}
static void writeall(int fd,const void* p,size_t n){auto b=(const char*)p;while(n){ssize_t x=send(fd,b,n,MSG_NOSIGNAL);if(x<0&&errno==EINTR)continue;ck(x>0,"send");b+=x;n-=x;}}
static bool readall(int fd,void* p,size_t n){auto b=(char*)p;size_t left=n;while(left){ssize_t x=recv(fd,b,left,0);if(x<0&&errno==EINTR)continue;if(x==0){ck(left==n,"partial stream EOF");return false;}ck(x>0,"recv");b+=x;left-=x;}return true;}
static sockaddr_in addr(){sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);return a;}
struct Pair{int tx=-1,rx=-1;int sndbuf=0,rcvbuf=0;};
static int sbuf(int fd,int op){int v=0;socklen_t l=sizeof(v);ck(getsockopt(fd,SOL_SOCKET,op,&v,&l)==0,"getsockopt");return v;}
static Pair pairmake(bool tcp){Pair p;int one=1,sz=2*1024*1024;int r=socket(AF_INET,tcp?SOCK_STREAM:SOCK_DGRAM,0);ck(r>=0,"socket");auto a=addr();ck(bind(r,(sockaddr*)&a,sizeof(a))==0,"bind");socklen_t al=sizeof(a);ck(getsockname(r,(sockaddr*)&a,&al)==0,"getsockname");
 if(tcp)ck(listen(r,1)==0,"listen");p.tx=socket(AF_INET,tcp?SOCK_STREAM:SOCK_DGRAM,0);ck(p.tx>=0,"socket tx");ck(connect(p.tx,(sockaddr*)&a,sizeof(a))==0,"connect");
 if(tcp){p.rx=accept(r,nullptr,nullptr);ck(p.rx>=0,"accept");close(r);setsockopt(p.tx,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));setsockopt(p.rx,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));}else p.rx=r;
 setsockopt(p.tx,SOL_SOCKET,SO_SNDBUF,&sz,sizeof(sz));setsockopt(p.rx,SOL_SOCKET,SO_RCVBUF,&sz,sizeof(sz));timeval to{5,0};setsockopt(p.tx,SOL_SOCKET,SO_SNDTIMEO,&to,sizeof(to));setsockopt(p.rx,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to));p.sndbuf=sbuf(p.tx,SO_SNDBUF);p.rcvbuf=sbuf(p.rx,SO_RCVBUF);return p;}
static void pairclose(Pair p){close(p.tx);close(p.rx);}
struct ZPair{void* ctx=nullptr;void* tx=nullptr;void* rx=nullptr;};
static ZPair zmake(bool reqrep){ZPair z;z.ctx=zmq_ctx_new();ck(z.ctx,"zmq_ctx_new");z.rx=zmq_socket(z.ctx,reqrep?ZMQ_REP:ZMQ_PULL);z.tx=zmq_socket(z.ctx,reqrep?ZMQ_REQ:ZMQ_PUSH);ck(z.rx&&z.tx,"zmq_socket");int hwm=8192,linger=0,timeout=5000,immediate=1;
 for(void* s:{z.tx,z.rx}){zmq_setsockopt(s,ZMQ_LINGER,&linger,sizeof(linger));zmq_setsockopt(s,ZMQ_SNDHWM,&hwm,sizeof(hwm));zmq_setsockopt(s,ZMQ_RCVHWM,&hwm,sizeof(hwm));zmq_setsockopt(s,ZMQ_SNDTIMEO,&timeout,sizeof(timeout));zmq_setsockopt(s,ZMQ_RCVTIMEO,&timeout,sizeof(timeout));}
 zmq_setsockopt(z.tx,ZMQ_IMMEDIATE,&immediate,sizeof(immediate));ck(zmq_bind(z.rx,"tcp://127.0.0.1:*")==0,"zmq_bind");char endpoint[256];size_t el=sizeof(endpoint);ck(zmq_getsockopt(z.rx,ZMQ_LAST_ENDPOINT,endpoint,&el)==0,"zmq endpoint");ck(zmq_connect(z.tx,endpoint)==0,"zmq_connect");zmq_pollitem_t poll{z.tx,0,ZMQ_POLLOUT,0};ck(zmq_poll(&poll,1,5000)>0,"zmq ready");return z;}
static void zclose(ZPair z){zmq_close(z.tx);zmq_close(z.rx);zmq_ctx_term(z.ctx);}
struct Result{uint64_t sent=0,received=0,errors=0,order_errors=0,checksum=0;double send_s=0,elapsed_s=0,send_cpu=0,recv_cpu=0,process_cpu=0;int sndbuf=0,rcvbuf=0;};
static Result throughput(const std::string& mode,int size,double seconds){bool tcp=mode.rfind("tcp",0)==0,zmq=mode=="zmq_push_pull";int batch=(mode=="tcp_batch32"||mode=="udp_mmsg32")?32:1;Result res;Pair p;ZPair z;if(zmq){z=zmake(false);int timeout=10;zmq_setsockopt(z.rx,ZMQ_RCVTIMEO,&timeout,sizeof(timeout));}else{p=pairmake(tcp);res.sndbuf=p.sndbuf;res.rcvbuf=p.rcvbuf;if(!tcp){timeval to{0,10000};setsockopt(p.rx,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to));}}
 std::barrier gate(3);std::atomic<bool> done{false};std::atomic<uint64_t> total{0};double started=0,send_end=0,recv_end=0;
 std::thread receiver([&]{pin(2);std::vector<char> buf(size_t(size)*32,0);std::vector<mmsghdr> msgs(32);std::vector<iovec> iovs(32);if(mode=="udp_mmsg32")for(int i=0;i<32;i++){iovs[i]={buf.data()+size_t(i)*size,size_t(size)};msgs[i].msg_hdr.msg_iov=&iovs[i];msgs[i].msg_hdr.msg_iovlen=1;}
  uint64_t expected=0;auto consume=[&](const char* b){uint64_t seq;memcpy(&seq,b,8);if(seq!=expected)res.order_errors++;expected=seq+1;res.checksum+=seq+(unsigned char)b[size-1];res.received++;};
  gate.arrive_and_wait();double c0=cput(CLOCK_THREAD_CPUTIME_ID);
  if(tcp){std::vector<char> stream(size_t(size+4)*128);size_t used=0;while(true){ssize_t n=recv(p.rx,stream.data()+used,stream.size()-used,0);if(n<0&&errno==EINTR)continue;ck(n>=0,"TCP buffered receive");if(n==0){ck(used==0,"partial framed EOF");break;}used+=n;size_t parsed=0;while(used-parsed>=4){uint32_t len;memcpy(&len,stream.data()+parsed,4);ck(ntohl(len)==(uint32_t)size,"framing length");if(used-parsed<size_t(size+4))break;consume(stream.data()+parsed+4);parsed+=size+4;}if(parsed){used-=parsed;memmove(stream.data(),stream.data()+parsed,used);recv_end=now();}}}
  else if(zmq){while(true){int n=zmq_recv(z.rx,buf.data(),size,0);if(n<0){if(errno==EAGAIN){if(done.load(std::memory_order_acquire)&&res.received==total.load())break;continue;}ck(false,"zmq_recv");}ck(n==size,"zmq size");consume(buf.data());recv_end=now();if(done.load(std::memory_order_acquire)&&res.received==total.load())break;}}
  else{while(true){int n;if(mode=="udp_mmsg32"){for(auto& m:msgs)m.msg_len=0;n=recvmmsg(p.rx,msgs.data(),32,MSG_WAITFORONE,nullptr);if(n>0){for(int i=0;i<n;i++){ck(msgs[i].msg_len==(unsigned)size,"udp size");consume(buf.data()+size_t(i)*size);}recv_end=now();}}else{n=recv(p.rx,buf.data(),size,0);if(n>0){ck(n==size,"udp size");consume(buf.data());recv_end=now();}}
    if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK){if(done.load(std::memory_order_acquire))break;continue;}ck(false,"udp receive");}}
  }
  res.recv_cpu=cput(CLOCK_THREAD_CPUTIME_ID)-c0;
 });
 std::thread sender([&]{pin(1);std::vector<char> body(size_t(size)*batch,'x'),framed(size_t(size+4)*batch,'x');std::vector<mmsghdr> msgs(batch);std::vector<iovec> iovs(batch);for(int i=0;i<batch;i++){iovs[i]={body.data()+size_t(i)*size,size_t(size)};msgs[i].msg_hdr.msg_iov=&iovs[i];msgs[i].msg_hdr.msg_iovlen=1;uint32_t l=htonl(size);memcpy(framed.data()+size_t(i)*(size+4),&l,4);}
  gate.arrive_and_wait();double c0=cput(CLOCK_THREAD_CPUTIME_ID);started=now();double deadline=started+seconds;
  do{if(tcp){for(int i=0;i<batch;i++){uint64_t s=res.sent+i;memcpy(framed.data()+size_t(i)*(size+4)+4,&s,8);}writeall(p.tx,framed.data(),framed.size());res.sent+=batch;}
   else if(zmq){memcpy(body.data(),&res.sent,8);ck(zmq_send(z.tx,body.data(),size,0)==size,"zmq_send");res.sent++;}
   else if(batch==32){for(int i=0;i<batch;i++){uint64_t s=res.sent+i;memcpy(body.data()+size_t(i)*size,&s,8);}int k=sendmmsg(p.tx,msgs.data(),batch,0);if(k<0){res.errors++;ck(false,"sendmmsg");}res.sent+=k;}
   else{memcpy(body.data(),&res.sent,8);int n=send(p.tx,body.data(),size,0);if(n<0){res.errors++;ck(false,"udp send");}ck(n==size,"udp send size");res.sent++;}
  }while(now()<deadline);
  send_end=now();res.send_cpu=cput(CLOCK_THREAD_CPUTIME_ID)-c0;total.store(res.sent,std::memory_order_release);done.store(true,std::memory_order_release);if(tcp)shutdown(p.tx,SHUT_WR);
 });
 double pc0=cput(CLOCK_PROCESS_CPUTIME_ID);gate.arrive_and_wait();sender.join();receiver.join();res.process_cpu=cput(CLOCK_PROCESS_CPUTIME_ID)-pc0;res.send_s=send_end-started;res.elapsed_s=std::max(send_end,recv_end)-started;
 if(zmq)zclose(z);else pairclose(p);ck(res.received<=res.sent,"received exceeds sent");if(tcp||zmq){ck(res.received==res.sent,"reliable mode lost records");ck(res.order_errors==0,"reliable mode reordered");uint64_t n=res.sent;uint64_t exp=(n*(n-1))/2+n*120;ck(res.checksum==exp,"checksum");}return res;
}
struct Rtt{std::vector<double> ns;double process_cpu=0;uint64_t timeouts=0;};
static Rtt rtt(const std::string& mode,int size,int warm,int n){bool tcp=mode=="tcp",zmq=mode=="zmq_req_rep";Pair p;ZPair z;Rtt r;if(zmq)z=zmake(true);else p=pairmake(tcp);
 if(!tcp&&!zmq){timeval to{0,100000};setsockopt(p.tx,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to));setsockopt(p.rx,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to));}
 std::atomic<bool> done{false};std::barrier gate(3);
 std::thread echo([&]{pin(2);std::vector<char>b(size),frame(size+4);int echoed=0;gate.arrive_and_wait();while(zmq?echoed<warm+n:!done.load(std::memory_order_acquire)){int got=0;if(tcp){uint32_t l;if(!readall(p.rx,&l,4))break;ck(ntohl(l)==(unsigned)size,"rtt framing");ck(readall(p.rx,b.data(),size),"rtt body");memcpy(frame.data(),&l,4);memcpy(frame.data()+4,b.data(),size);writeall(p.rx,frame.data(),frame.size());}
  else if(zmq){got=zmq_recv(z.rx,b.data(),size,0);if(got<0){if(done.load())break;ck(false,"rtt zmq_recv");}ck(zmq_send(z.rx,b.data(),got,0)==got,"rtt zmq_send");echoed++;}
  else{sockaddr_in from{};socklen_t fl=sizeof(from);got=recvfrom(p.rx,b.data(),size,0,(sockaddr*)&from,&fl);if(got<0){if(errno==EAGAIN||errno==EWOULDBLOCK)continue;ck(false,"rtt udp recvfrom");}ck(sendto(p.rx,b.data(),got,0,(sockaddr*)&from,fl)==got,"rtt udp echo");}}
 });
 std::thread client([&]{pin(1);std::vector<char>b(size,'x'),back(size),frame(size+4);uint32_t l=htonl(size);r.ns.reserve(n);gate.arrive_and_wait();for(int i=0;i<warm+n;i++){uint64_t seq=i;memcpy(b.data(),&seq,8);double st=now();int got=size;
   if(tcp){memcpy(frame.data(),&l,4);memcpy(frame.data()+4,b.data(),size);writeall(p.tx,frame.data(),frame.size());uint32_t len;ck(readall(p.tx,&len,4)&&ntohl(len)==(unsigned)size,"rtt reply frame");ck(readall(p.tx,back.data(),size),"rtt reply body");}
   else if(zmq){ck(zmq_send(z.tx,b.data(),size,0)==size,"rtt req");got=zmq_recv(z.tx,back.data(),size,0);ck(got==size,"rtt rep");}
   else{ck(send(p.tx,b.data(),size,0)==size,"rtt udp send");got=recv(p.tx,back.data(),size,0);if(got<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){r.timeouts++;continue;}ck(got==size,"rtt udp reply");}
   double en=now();ck(memcmp(back.data(),b.data(),size)==0,"rtt payload");if(i>=warm)r.ns.push_back((en-st)*1e9);
  }done.store(true,std::memory_order_release);if(tcp)shutdown(p.tx,SHUT_WR);
 });double c0=cput(CLOCK_PROCESS_CPUTIME_ID);gate.arrive_and_wait();client.join();echo.join();r.process_cpu=cput(CLOCK_PROCESS_CPUTIME_ID)-c0;if(zmq)zclose(z);else pairclose(p);std::sort(r.ns.begin(),r.ns.end());return r;}
static double q(const std::vector<double>& v,double p){return v.at(std::min(v.size()-1,size_t(p*(v.size()-1))));}
static uint64_t nowns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
struct Paced{
 uint64_t offered=0,attempted=0,accepted=0,received=0,eagain=0,other_errors=0,schedule_missed=0,max_behind_slots=0;
 double elapsed_s=0,send_cpu=0,recv_cpu=0,process_cpu=0;
 std::vector<double> planned_us,actual_us,lateness_us;
};
static Paced paced(uint64_t rate,double seconds){Paced r;r.offered=uint64_t(rate*seconds);r.planned_us.resize(r.offered);r.actual_us.resize(r.offered);r.lateness_us.resize(r.offered);auto p=pairmake(false);timeval timeout{0,10000};setsockopt(p.rx,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 std::barrier gate(3);std::atomic<bool> done{false};uint64_t base=0,send_end=0,last_recv=0;
 std::thread rx([&]{pin(2);std::vector<char>b(256);gate.arrive_and_wait();double c0=cput(CLOCK_THREAD_CPUTIME_ID);while(true){ssize_t n=recv(p.rx,b.data(),b.size(),0);if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK){if(done.load(std::memory_order_acquire))break;continue;}if(errno==EINTR)continue;ck(false,"paced recv");}uint64_t arrived=nowns();ck(n==256,"paced size");uint64_t planned,actual;memcpy(&planned,b.data()+8,8);memcpy(&actual,b.data()+16,8);ck(arrived>=actual&&actual>=planned,"paced clock order");ck(r.received<r.offered,"paced count");r.planned_us[r.received]=(arrived-planned)/1000.0;r.actual_us[r.received]=(arrived-actual)/1000.0;r.received++;last_recv=arrived;}r.recv_cpu=cput(CLOCK_THREAD_CPUTIME_ID)-c0;});
 std::thread tx([&]{pin(1);std::vector<char>b(256,'x');gate.arrive_and_wait();double c0=cput(CLOCK_THREAD_CPUTIME_ID);base=nowns()+1000000;uint64_t end=base+uint64_t(seconds*1e9);for(uint64_t i=0;i<r.offered;i++){uint64_t planned=base+(i*1000000000ull)/rate;uint64_t actual;do{actual=nowns();if(actual<planned)asm volatile("pause");}while(actual<planned);if(actual>=end)break;memcpy(b.data(),&i,8);memcpy(b.data()+8,&planned,8);memcpy(b.data()+16,&actual,8);r.lateness_us[r.attempted]=(actual-planned)/1000.0;r.max_behind_slots=std::max<uint64_t>(r.max_behind_slots,(actual-planned)*rate/1000000000ull);r.attempted++;ssize_t n=send(p.tx,b.data(),b.size(),MSG_DONTWAIT);if(n==256)r.accepted++;else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))r.eagain++;else r.other_errors++;}send_end=nowns();r.send_cpu=cput(CLOCK_THREAD_CPUTIME_ID)-c0;r.schedule_missed=r.offered-r.attempted;done.store(true,std::memory_order_release);});
 double c0=cput(CLOCK_PROCESS_CPUTIME_ID);gate.arrive_and_wait();tx.join();rx.join();r.process_cpu=cput(CLOCK_PROCESS_CPUTIME_ID)-c0;r.elapsed_s=(std::max(send_end,last_recv)-base)/1e9;pairclose(p);ck(r.received<=r.accepted,"paced received count");r.planned_us.resize(r.received);r.actual_us.resize(r.received);r.lateness_us.resize(r.attempted);std::sort(r.planned_us.begin(),r.planned_us.end());std::sort(r.actual_us.begin(),r.actual_us.end());std::sort(r.lateness_us.begin(),r.lateness_us.end());return r;}
static int runpaced(const std::string& out,double seconds,int reps){std::ofstream f(out+"_paced.csv");f<<"target_msg_s,body_bytes,planned_seconds,repeat,offered_slots,attempted,accepted,received,source_schedule_missed,send_eagain,send_other_errors,accepted_minus_received,offered_minus_received,delivery_seconds,received_msg_s,arrival_minus_planned_p50_us,arrival_minus_planned_p95_us,arrival_minus_planned_p99_us,arrival_minus_actual_p50_us,arrival_minus_actual_p99_us,sender_lateness_p50_us,sender_lateness_p95_us,sender_lateness_p99_us,sender_lateness_max_us,max_behind_schedule_slots,sender_cpu_s,receiver_cpu_s,process_cpu_s\n";f<<std::setprecision(12);auto quant=[](const std::vector<double>& v,double p){return v.empty()?0.0:q(v,p);};for(uint64_t rate:{50000,200000,500000,1000000})for(int rep=0;rep<reps;rep++){auto r=paced(rate,seconds);f<<rate<<",256,"<<seconds<<','<<rep<<','<<r.offered<<','<<r.attempted<<','<<r.accepted<<','<<r.received<<','<<r.schedule_missed<<','<<r.eagain<<','<<r.other_errors<<','<<r.accepted-r.received<<','<<r.offered-r.received<<','<<r.elapsed_s<<','<<r.received/r.elapsed_s<<','<<quant(r.planned_us,.50)<<','<<quant(r.planned_us,.95)<<','<<quant(r.planned_us,.99)<<','<<quant(r.actual_us,.50)<<','<<quant(r.actual_us,.99)<<','<<quant(r.lateness_us,.50)<<','<<quant(r.lateness_us,.95)<<','<<quant(r.lateness_us,.99)<<','<<quant(r.lateness_us,1)<<','<<r.max_behind_slots<<','<<r.send_cpu<<','<<r.recv_cpu<<','<<r.process_cpu<<'\n';f.flush();std::cerr<<"PACED target="<<rate<<" rep="<<rep<<" recv="<<r.received<<" accepted="<<r.accepted<<" schedule_missed="<<r.schedule_missed<<" p99_planned_us="<<quant(r.planned_us,.99)<<" p99_sendlate_us="<<quant(r.lateness_us,.99)<<'\n';}return 0;}

int main(int argc,char** argv){try{std::string out="results";double seconds=.6;int reps=5,rounds=5000;bool paced_only=false;for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--out"&&i+1<argc)out=argv[++i];else if(a=="--seconds"&&i+1<argc)seconds=atof(argv[++i]);else if(a=="--repeats"&&i+1<argc)reps=atoi(argv[++i]);else if(a=="--rounds"&&i+1<argc)rounds=atoi(argv[++i]);else if(a=="--paced")paced_only=true;else throw std::runtime_error("args: --out PREFIX --seconds N --repeats N --rounds N [--paced]");}if(paced_only)return runpaced(out,seconds,reps);
 std::ofstream f(out+"_throughput.csv");f<<"mode,body_bytes,tcp_frame_bytes,repeat,sent,received,missing,sender_errors,sequence_discontinuities,send_seconds,delivery_seconds,received_mmsg_s,sender_cpu_s,receiver_cpu_s,process_cpu_s,process_cpu_cores,sndbuf_actual,rcvbuf_actual\n";f<<std::setprecision(12);
 int za,zb,zc;zmq_version(&za,&zb,&zc);std::cerr<<"libzmq="<<za<<'.'<<zb<<'.'<<zc<<" sender_cpu=1 receiver_cpu=2 process CPU includes ZeroMQ I/O thread\n";
 for(int size:{64,256,1024})for(std::string mode:{"tcp_record","tcp_batch32","udp_record","udp_mmsg32","zmq_push_pull"}){throughput(mode,size,.1);for(int rep=0;rep<reps;rep++){auto r=throughput(mode,size,seconds);f<<mode<<','<<size<<','<<(mode.rfind("tcp",0)==0?4:0)<<','<<rep<<','<<r.sent<<','<<r.received<<','<<r.sent-r.received<<','<<r.errors<<','<<r.order_errors<<','<<r.send_s<<','<<r.elapsed_s<<','<<r.received/r.elapsed_s/1e6<<','<<r.send_cpu<<','<<r.recv_cpu<<','<<r.process_cpu<<','<<r.process_cpu/r.elapsed_s<<','<<r.sndbuf<<','<<r.rcvbuf<<'\n';f.flush();std::cerr<<mode<<" bytes="<<size<<" rep="<<rep<<" delivered="<<r.received/r.elapsed_s/1e6<<" M/s missing="<<r.sent-r.received<<'\n';}}
 std::ofstream g(out+"_rtt.csv");g<<"mode,body_bytes,repeat,samples,timeouts,p50_us,p95_us,p99_us,max_us,process_cpu_s\n";g<<std::setprecision(12);for(int size:{64,256,1024})for(std::string mode:{"tcp","udp","zmq_req_rep"})for(int rep=0;rep<reps;rep++){auto r=rtt(mode,size,1000,rounds);g<<mode<<','<<size<<','<<rep<<','<<r.ns.size()<<','<<r.timeouts<<','<<q(r.ns,.50)/1000<<','<<q(r.ns,.95)/1000<<','<<q(r.ns,.99)/1000<<','<<r.ns.back()/1000<<','<<r.process_cpu<<'\n';g.flush();std::cerr<<"RTT "<<mode<<" bytes="<<size<<" rep="<<rep<<" p50_us="<<q(r.ns,.50)/1000<<" p99_us="<<q(r.ns,.99)/1000<<'\n';}
 return 0;}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}}
