import java.util.*;

/** Sequential, prebuilt-input decode microbenchmark, not JMH or a network test. */
public final class JavaCodecBench {
    static volatile long blackhole;
    public static void main(String[] args) throws Exception {
        String corpus=args.length>0?args[0]:"../codec/corpus";
        int reps=args.length>1?Integer.parseInt(args[1]):5;
        int iterations=args.length>2?Integer.parseInt(args[2]):2_097_152;
        System.out.println("runtime,mode,payload_bytes,repeat,iterations,ns_per_op,checksum,wire_bytes_mean");
        for(int payload: new int[]{0,64,256,1024}) {
            WireCorpus fixed=new WireCorpus(corpus,"fixed",payload,true), pb=new WireCorpus(corpus,"protobuf",payload,false);
            for(int i=0;i<fixed.bytes.length;i++) if(fixed.sum(i,"heap")!=pb.sum(i,"protobuf")) throw new AssertionError("codec mismatch");
            for(String mode:new String[]{"heap","direct","protobuf"}) {
                WireCorpus c=mode.equals("protobuf")?pb:fixed;
                for(int warm=0;warm<5;warm++) {long v=0;for(int i=0;i<262144;i++)v+=c.sum(i&(c.bytes.length-1),mode);blackhole=v;}
                for(int rep=0;rep<reps;rep++) {
                    long sum=0,t=System.nanoTime();
                    for(int i=0;i<iterations;i++)sum+=c.sum(i&(c.bytes.length-1),mode);
                    long elapsed=System.nanoTime()-t;blackhole=sum;
                    double bytes=Arrays.stream(c.bytes).mapToInt(a->a.length).average().orElseThrow();
                    System.out.printf(Locale.ROOT,"java21,%s,%d,%d,%d,%.4f,%s,%.4f%n",mode,payload,rep,iterations,elapsed/(double)iterations,Long.toUnsignedString(sum),bytes);
                }
            }
        }
    }
}
