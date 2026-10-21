import java.io.*;
import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import org.apache.flink.api.common.*;
import org.apache.flink.api.common.accumulators.*;
import org.apache.flink.api.common.eventtime.WatermarkStrategy;
import org.apache.flink.api.common.functions.*;
import org.apache.flink.api.connector.source.*;
import org.apache.flink.api.java.tuple.Tuple4;
import org.apache.flink.configuration.Configuration;
import org.apache.flink.core.io.*;
import org.apache.flink.streaming.api.environment.StreamExecutionEnvironment;
import org.apache.flink.streaming.api.datastream.DataStream;
import org.apache.flink.streaming.api.functions.sink.legacy.RichSinkFunction;

/** Real local Flink MiniCluster: FLIP-27 bounded source -> map -> optional keyBy/reduce -> sink.
 * Input bytes prebuilt by C++; source projects full scalar/payload-edge checksum into a tuple.
 * This is CPU/serialization/shuffle overhead; there is no live UDP, DPDK or remote host. */
public final class FlinkLocalBench {
    public static final class Split implements SourceSplit, Serializable {
        final int id; final long count; long position;
        Split(int id,long count,long position){this.id=id;this.count=count;this.position=position;}
        public String splitId(){return Integer.toString(id);}
    }
    static final class SplitSer implements SimpleVersionedSerializer<Split> {
        public int getVersion(){return 1;}
        public byte[] serialize(Split s){return ByteBuffer.allocate(20).putInt(s.id).putLong(s.count).putLong(s.position).array();}
        public Split deserialize(int version,byte[] a) throws IOException {if(version!=1||a.length!=20)throw new IOException("split");ByteBuffer b=ByteBuffer.wrap(a);return new Split(b.getInt(),b.getLong(),b.getLong());}
    }
    static final class CheckpointSer implements SimpleVersionedSerializer<List<Split>> {
        public int getVersion(){return 1;}
        public byte[] serialize(List<Split> a) throws IOException {ByteArrayOutputStream out=new ByteArrayOutputStream();DataOutputStream d=new DataOutputStream(out);d.writeInt(a.size());for(Split s:a)d.write(new SplitSer().serialize(s));return out.toByteArray();}
        public List<Split> deserialize(int version,byte[] a) throws IOException {if(version!=1)throw new IOException("checkpoint");DataInputStream d=new DataInputStream(new ByteArrayInputStream(a));int n=d.readInt();List<Split> list=new ArrayList<>();for(int i=0;i<n;i++)list.add(new SplitSer().deserialize(1,d.readNBytes(20)));return list;}
    }
    static final class Enumer implements SplitEnumerator<Split,List<Split>> {
        final SplitEnumeratorContext<Split> ctx; final ArrayList<Split> pending;
        Enumer(SplitEnumeratorContext<Split> ctx,List<Split> s){this.ctx=ctx;this.pending=new ArrayList<>(s);}
        public void start(){}
        public void handleSplitRequest(int subtask,String host){assign(subtask);}
        public void addReader(int subtask){assign(subtask);}
        void assign(int subtask){for(Iterator<Split> it=pending.iterator();it.hasNext();){Split s=it.next();if(s.id==subtask){ctx.assignSplit(s,subtask);it.remove();}}ctx.signalNoMoreSplits(subtask);}
        public void addSplitsBack(List<Split> s,int subtask){pending.addAll(s);}
        public List<Split> snapshotState(long checkpointId){return new ArrayList<>(pending);}
        public void close(){}
    }
    static final class Reader implements SourceReader<Tuple4<Long,Long,Long,Long>,Split> {
        final String corpusDir,codec; final int payload;
        WireCorpus corpus; final ArrayDeque<Split> pending=new ArrayDeque<>();
        boolean noMore; CompletableFuture<Void> available=new CompletableFuture<>(); long firstNs;
        Reader(String d,String c,int p){corpusDir=d;codec=c;payload=p;}
        public void start(){try{corpus=new WireCorpus(corpusDir,codec,payload,false);}catch(IOException e){throw new RuntimeException(e);}}
        public InputStatus pollNext(ReaderOutput<Tuple4<Long,Long,Long,Long>> out) throws Exception {
            while(!pending.isEmpty()&&pending.peek().position>=pending.peek().count)pending.remove();
            if(pending.isEmpty())return noMore?InputStatus.END_OF_INPUT:InputStatus.NOTHING_AVAILABLE;
            Split s=pending.peek();int i=(int)(s.position&(corpus.bytes.length-1));
            long id,checksum;
            if(codec.equals("protobuf")) {bench.EventOuterClass.Event m=bench.EventOuterClass.Event.parseFrom(corpus.bytes[i]);id=m.getId();checksum=WireCorpus.protobufSum(m);}
            else {ByteBuffer b=corpus.heap[i];id=b.getLong(0);checksum=WireCorpus.fixedSum(b);}
            long stamp=0;if(firstNs==0){firstNs=System.nanoTime();stamp=firstNs;}
            out.collect(Tuple4.of(id&1023L,checksum,1L,stamp));s.position++;
            return InputStatus.MORE_AVAILABLE;
        }
        public List<Split> snapshotState(long id){return new ArrayList<>(pending);}
        public CompletableFuture<Void> isAvailable(){return pending.isEmpty()&&!noMore?available:CompletableFuture.completedFuture(null);}
        public void addSplits(List<Split> a){pending.addAll(a);available.complete(null);}
        public void notifyNoMoreSplits(){noMore=true;available.complete(null);}
        public void close(){}
    }
    static final class Src implements Source<Tuple4<Long,Long,Long,Long>,Split,List<Split>> {
        final String dir,codec;final int payload;final long count;
        Src(String d,String c,int p,long n){dir=d;codec=c;payload=p;count=n;}
        public Boundedness getBoundedness(){return Boundedness.BOUNDED;}
        public SourceReader<Tuple4<Long,Long,Long,Long>,Split> createReader(SourceReaderContext c){return new Reader(dir,codec,payload);}
        public SplitEnumerator<Split,List<Split>> createEnumerator(SplitEnumeratorContext<Split> c){int p=c.currentParallelism();ArrayList<Split>s=new ArrayList<>();for(int i=0;i<p;i++)s.add(new Split(i,count/p+(i<count%p?1:0),0));return new Enumer(c,s);}
        public SplitEnumerator<Split,List<Split>> restoreEnumerator(SplitEnumeratorContext<Split> c,List<Split> s){return new Enumer(c,s);}
        public SimpleVersionedSerializer<Split> getSplitSerializer(){return new SplitSer();}
        public SimpleVersionedSerializer<List<Split>> getEnumeratorCheckpointSerializer(){return new CheckpointSer();}
    }
    static final class CountingSink extends RichSinkFunction<Tuple4<Long,Long,Long,Long>> {
        long n,sum,first=Long.MAX_VALUE;
        final LongCounter count=new LongCounter(),check=new LongCounter();final LongMinimum begin=new LongMinimum();final LongMaximum end=new LongMaximum();
        public void open(OpenContext c){getRuntimeContext().addAccumulator("records",count);getRuntimeContext().addAccumulator("checksum",check);getRuntimeContext().addAccumulator("first_source_ns",begin);getRuntimeContext().addAccumulator("sink_close_ns",end);}
        public void invoke(Tuple4<Long,Long,Long,Long> r,Context c){n++;sum+=r.f1;if(r.f3!=0)first=Math.min(first,r.f3);}
        public void close(){count.add(n);check.add(sum);if(first!=Long.MAX_VALUE)begin.add(first);end.add(System.nanoTime());}
    }
    public static void main(String[] args) throws Exception {
        String corpus=args.length>0?args[0]:"../codec/corpus";
        String codec=args.length>1?args[1]:"fixed";
        String mode=args.length>2?args[2]:"keyby";
        int parallel=args.length>3?Integer.parseInt(args[3]):2;
        long n=args.length>4?Long.parseLong(args[4]):3_000_000L;
        int reps=args.length>5?Integer.parseInt(args[5]):3;
        int payload=args.length>6?Integer.parseInt(args[6]):64;
        System.out.println("runtime,codec,mode,parallelism,payload_bytes,repeat,records,job_runtime_ms,wall_ms,data_window_ms,data_window_mps,checksum");
        for(int rep=-1;rep<reps;rep++) {
            Configuration config=new Configuration();config.setString("rest.bind-address","127.0.0.1");config.setString("jobmanager.bind-host","127.0.0.1");config.setString("taskmanager.bind-host","127.0.0.1");config.setString("taskmanager.memory.network.min","64m");config.setString("taskmanager.memory.network.max","64m");config.setString("taskmanager.memory.managed.size","64m");config.setString("taskmanager.numberOfTaskSlots",Integer.toString(parallel));
            StreamExecutionEnvironment env=StreamExecutionEnvironment.createLocalEnvironment(parallel,config);
            env.setRuntimeMode(RuntimeExecutionMode.STREAMING);
            env.setParallelism(parallel);
            DataStream<Tuple4<Long,Long,Long,Long>> stream=env.fromSource(new Src(corpus,codec,payload,n),WatermarkStrategy.noWatermarks(),"fixed-wire FLIP27 source").returns(new org.apache.flink.api.common.typeinfo.TypeHint<Tuple4<Long,Long,Long,Long>>(){});
            stream=stream.map(r->Tuple4.of(r.f0,r.f1+7,r.f2,r.f3)).returns(new org.apache.flink.api.common.typeinfo.TypeHint<Tuple4<Long,Long,Long,Long>>(){});
            if(mode.equals("keyby")) stream=stream.keyBy(r->r.f0).reduce((a,b)->Tuple4.of(b.f0,a.f1+b.f1,a.f2+b.f2,a.f3==0?b.f3:b.f3==0?a.f3:Math.min(a.f3,b.f3)));
            stream.addSink(new CountingSink()).name("count-checksum-discard");
            long start=System.nanoTime();JobExecutionResult result=env.execute("wire-local-benchmark");long wall=System.nanoTime()-start;
            long count=result.<Long>getAccumulatorResult("records"),first=result.<Long>getAccumulatorResult("first_source_ns"),last=result.<Long>getAccumulatorResult("sink_close_ns"),checksum=result.<Long>getAccumulatorResult("checksum");
            if(count!=n||last<=first)throw new AssertionError("count/timer "+count+" "+first+" "+last);
            if(rep>=0)System.out.printf(Locale.ROOT,"flink2.1.0,%s,%s,%d,%d,%d,%d,%d,%.4f,%.4f,%.6f,%s%n",codec,mode,parallel,payload,rep,count,result.getNetRuntime(),wall/1e6,(last-first)/1e6,n*1e3/(last-first),Long.toUnsignedString(checksum));
        }
    }
}
