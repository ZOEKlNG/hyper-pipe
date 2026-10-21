import java.nio.*;
import java.nio.channels.*;
import java.net.*;
import java.nio.file.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class UdpReceiver {
 static final int HEADER=72,MAX_DATAGRAM=1472,MAGIC=0x31475048;
 static final class Slot { final ByteBuffer b=ByteBuffer.allocateDirect(MAX_DATAGRAM+1).order(ByteOrder.LITTLE_ENDIAN); }
 static boolean valid(ByteBuffer b) {
  int n=b.remaining();if(n<HEADER||n>MAX_DATAGRAM)return false;
  if(b.getInt(0)!=MAGIC||b.getShort(4)!=1||b.getInt(12)!=1)return false;
  int h=Short.toUnsignedInt(b.getShort(6));if(h<HEADER||h>n||b.getInt(8)!=n)return false;
  return b.getInt(68)==n-h;
 }
 static long consume(ByteBuffer b) {
  long s=0;for(int off=16;off<56;off+=8)s+=b.getLong(off);
  for(int off=56;off<72;off+=4)s+=Integer.toUnsignedLong(b.getInt(off));
  int h=Short.toUnsignedInt(b.getShort(6));for(int i=h;i<b.limit();i++)s+=Byte.toUnsignedInt(b.get(i));return s;
 }
 static void selftest(String dir)throws Exception{
  for(String name:new String[]{"valid.bin","bad-version.bin","bad-length.bin","bad-header.bin","bad-magic.bin"}){
   ByteBuffer b=ByteBuffer.wrap(Files.readAllBytes(Path.of(dir,name))).order(ByteOrder.LITTLE_ENDIAN);
   boolean ok=valid(b),expected=name.equals("valid.bin");if(ok!=expected)throw new AssertionError(name);
   if(ok&&(b.getLong(40)!=123456||b.getInt(68)!=256||Byte.toUnsignedInt(b.get(72))!=64))throw new AssertionError("cross-language fields");
  }System.out.println("Java cross-language protocol tests PASS");
 }
 public static void main(String[]args)throws Exception{
  if(args.length==2&&args[0].equals("--selftest")){selftest(args[1]);return;}
  int port=19090,capacity=1024,seconds=5,delayUs=0;String host="0.0.0.0";
  for(int i=0;i<args.length;i++){String k=args[i];if(i+1>=args.length)throw new IllegalArgumentException("missing value: "+k);String v=args[++i];switch(k){case "--host":host=v;break;case "--port":port=Integer.parseInt(v);break;case "--queue":capacity=Integer.parseInt(v);break;case "--seconds":seconds=Integer.parseInt(v);break;case "--worker-delay-us":delayUs=Integer.parseInt(v);break;default:throw new IllegalArgumentException(k);}}
  if(port<1||port>65535||capacity<1||capacity>1048576||seconds<1||delayUs<0)throw new IllegalArgumentException("bounded configuration");
  ArrayBlockingQueue<Slot>pool=new ArrayBlockingQueue<>(capacity+2),queue=new ArrayBlockingQueue<>(capacity);for(int i=0;i<capacity+2;i++)pool.add(new Slot());Slot discard=new Slot();
  AtomicBoolean running=new AtomicBoolean(true),receiverDone=new AtomicBoolean(false);LongAdder received=new LongAdder(),invalid=new LongAdder(),queued=new LongAdder(),queueDrop=new LongAdder(),poolEmpty=new LongAdder(),processed=new LongAdder();AtomicLong checksum=new AtomicLong();AtomicReference<Throwable> failure=new AtomicReference<>();
  DatagramChannel ch=DatagramChannel.open(StandardProtocolFamily.INET);ch.bind(new InetSocketAddress(host,port));ch.configureBlocking(true);ch.setOption(StandardSocketOptions.SO_RCVBUF,4*1024*1024);int actualRcvBuf=ch.getOption(StandardSocketOptions.SO_RCVBUF);final int workerDelayUs=delayUs;
  Thread worker=Thread.ofPlatform().name("consumer").start(()->{long sum=0;try{while(!receiverDone.get()||!queue.isEmpty()){Slot s=queue.poll(20,TimeUnit.MILLISECONDS);if(s==null)continue;sum+=consume(s.b);if(workerDelayUs>0)TimeUnit.MICROSECONDS.sleep(workerDelayUs);processed.increment();if(!pool.offer(s))throw new AssertionError("pool ownership");}}catch(InterruptedException e){Thread.currentThread().interrupt();failure.compareAndSet(null,e);}catch(Throwable e){failure.compareAndSet(null,e);}checksum.set(sum);});
  Thread receiver=Thread.ofPlatform().name("udp-drain").start(()->{try{while(running.get()){Slot s=pool.poll();boolean owned=s!=null;if(!owned){s=discard;poolEmpty.increment();}s.b.clear();SocketAddress remote=ch.receive(s.b);if(remote==null){if(owned)pool.add(s);continue;}s.b.flip();received.increment();if(!valid(s.b)){invalid.increment();if(owned)pool.add(s);continue;}if(!owned){queueDrop.increment();continue;}if(queue.offer(s))queued.increment();else{queueDrop.increment();pool.add(s);}}}catch(ClosedChannelException e){if(running.get())failure.compareAndSet(null,e);}catch(Exception e){failure.compareAndSet(null,e);running.set(false);}finally{receiverDone.set(true);}});
  System.out.println("READY port="+port+" queue="+capacity+" socket_rcvbuf="+actualRcvBuf);System.out.flush();long start=System.nanoTime();TimeUnit.SECONDS.sleep(seconds);running.set(false);ch.close();receiver.join();worker.join();if(failure.get()!=null)throw new RuntimeException("worker failure",failure.get());double elapsed=(System.nanoTime()-start)/1e9;
  long rx=received.sum(),bad=invalid.sum(),q=queued.sum(),drop=queueDrop.sum(),done=processed.sum();if(rx!=bad+q+drop||q!=done)throw new AssertionError("counter conservation");
  System.out.println("{\"received\":"+rx+",\"invalid\":"+bad+",\"queued\":"+q+",\"queue_drop\":"+drop+",\"pool_empty_observed\":"+poolEmpty.sum()+",\"processed\":"+done+",\"checksum\":"+checksum.get()+",\"elapsed_s\":"+elapsed+",\"socket_rcvbuf\":"+actualRcvBuf+",\"queue_capacity\":"+capacity+"}");
 }
}
