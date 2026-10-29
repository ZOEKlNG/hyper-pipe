package interop;

import bench.EventOuterClass;
import com.google.protobuf.ByteString;
import com.google.protobuf.InvalidProtocolBufferException;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Independent correctness checker; deliberately not a timing benchmark. */
public final class InteropCheck {
    static final String[] NAMES = {"id","seq","timestamp","value0","value1","producer","stream","type","flags"};
    static final class Decoded {
        final long[] scalar = new long[9];
        byte[] payload = new byte[0];
    }
    static Decoded fromEvent(EventOuterClass.Event x) {
        Decoded d=new Decoded();
        d.scalar[0]=x.getId();d.scalar[1]=x.getSeq();d.scalar[2]=x.getTimestamp();d.scalar[3]=x.getValue0();d.scalar[4]=x.getValue1();
        d.scalar[5]=Integer.toUnsignedLong(x.getProducer());d.scalar[6]=Integer.toUnsignedLong(x.getStream());d.scalar[7]=Integer.toUnsignedLong(x.getType());d.scalar[8]=Integer.toUnsignedLong(x.getFlags());
        d.payload=x.getPayload().toByteArray();return d;
    }
    static Decoded fromFixed(EventOuterClass.FixedEvent x) {
        Decoded d=new Decoded();
        d.scalar[0]=x.getId();d.scalar[1]=x.getSeq();d.scalar[2]=x.getTimestamp();d.scalar[3]=x.getValue0();d.scalar[4]=x.getValue1();
        d.scalar[5]=Integer.toUnsignedLong(x.getProducer());d.scalar[6]=Integer.toUnsignedLong(x.getStream());d.scalar[7]=Integer.toUnsignedLong(x.getType());d.scalar[8]=Integer.toUnsignedLong(x.getFlags());
        d.payload=x.getPayload().toByteArray();return d;
    }
    static boolean fixed(String type) {
        if(type.equals("FixedEvent")||type.equals("bench.FixedEvent"))return true;
        if(type.equals("Event")||type.equals("bench.Event"))return false;
        throw new IllegalArgumentException("unsupported protobuf_type: "+type);
    }
    static Decoded protobuf(byte[] wire,String type)throws InvalidProtocolBufferException {
        return fixed(type)?fromFixed(EventOuterClass.FixedEvent.parseFrom(wire)):fromEvent(EventOuterClass.Event.parseFrom(wire));
    }
    static int u16(byte[] a,int p) {return (a[p]&255)|((a[p+1]&255)<<8);}
    static long uintLE(byte[] a,int p,int width) {
        long x=0;for(int i=0;i<width;i++)x|=((long)a[p+i]&255L)<<(i*8);return x;
    }
    static void require(int pos,int count,int limit) {
        if(pos<0||count<0||pos>limit||count>limit-pos)throw new IllegalArgumentException("truncated field");
    }
    /** TV/TLV format: LE u16 (type<<14|fid), then LE u32/u64 or u16 byte length + bytes. */
    static Decoded hybrid(byte[] wire) {
        if(wire.length>1024*1024)throw new IllegalArgumentException("frame exceeds 1MiB");
        Decoded d=new Decoded();int p=0;
        while(p<wire.length) {
            require(p,2,wire.length);int tag=u16(wire,p);p+=2;
            int type=tag>>>14,fid=tag&0x3fff;
            if(type==3||fid==0)throw new IllegalArgumentException("unsupported tag");
            int expected=fid<=5?1:fid<=9?0:fid==10?2:-1;
            if(expected>=0&&type!=expected)throw new IllegalArgumentException("known field type mismatch");
            if(type==0||type==1) {
                int width=type==0?4:8;require(p,width,wire.length);
                if(fid<=9)d.scalar[fid-1]=uintLE(wire,p,width);
                p+=width;
            } else {
                require(p,2,wire.length);int len=u16(wire,p);p+=2;require(p,len,wire.length);
                if(fid==10)d.payload=Arrays.copyOfRange(wire,p,p+len);
                p+=len;
            }
        }
        return d;
    }
    static void equal(Decoded expected,Decoded actual,String context) {
        for(int i=0;i<9;i++)if(expected.scalar[i]!=actual.scalar[i])throw new AssertionError(context+" field "+NAMES[i]+": expected "+Long.toUnsignedString(expected.scalar[i])+" got "+Long.toUnsignedString(actual.scalar[i]));
        if(!Arrays.equals(expected.payload,actual.payload))throw new AssertionError(context+" payload mismatch");
    }
    static byte[] join(byte[] a,byte[] b) {byte[] r=Arrays.copyOf(a,a.length+b.length);System.arraycopy(b,0,r,a.length,b.length);return r;}
    interface Throws {void run() throws Exception;}
    static void reject(String name,Throws action)throws Exception {
        try{action.run();}catch(IllegalArgumentException|InvalidProtocolBufferException expected){System.out.println("REJECT\t"+name);return;}
        throw new AssertionError("accepted malformed input: "+name);
    }
    static int standaloneProbes()throws Exception {
        reject("protobuf_incomplete_varint",()->EventOuterClass.Event.parseFrom(new byte[]{8,(byte)128}));
        reject("protobuf_truncated_bytes",()->EventOuterClass.Event.parseFrom(new byte[]{82,3,1}));
        reject("protobuf_zero_tag",()->EventOuterClass.Event.parseFrom(new byte[]{0}));
        reject("protobuf_fixed64_truncated",()->EventOuterClass.FixedEvent.parseFrom(new byte[]{9,1,2,3,4,5,6,7}));
        reject("hybrid_half_tag",()->hybrid(new byte[]{1}));
        reject("hybrid_zero_fid",()->hybrid(new byte[]{0,0,1,2,3,4}));
        reject("hybrid_reserved_type",()->hybrid(new byte[]{1,(byte)192}));
        reject("hybrid_wrong_known_type",()->hybrid(new byte[]{1,0,1,2,3,4}));
        reject("hybrid_truncated_scalar",()->hybrid(new byte[]{1,64,1,2,3}));
        reject("hybrid_missing_length",()->hybrid(new byte[]{10,(byte)128,1}));
        reject("hybrid_truncated_bytes",()->hybrid(new byte[]{10,(byte)128,2,0,1}));
        reject("hybrid_frame_limit",()->hybrid(new byte[1024*1024+1]));
        reject("hybrid_unknown_bytes_truncated",()->hybrid(new byte[]{(byte)255,(byte)191,2,0,1}));
        byte[] dup={1,64,5,0,0,0,0,0,0,0,1,64,7,0,0,0,0,0,0,0};
        if(hybrid(dup).scalar[0]!=7)throw new AssertionError("hybrid duplicate must use last value");
        byte[] dupPayload={10,(byte)128,1,0,1,10,(byte)128,2,0,(byte)255,0};
        if(!Arrays.equals(hybrid(dupPayload).payload,new byte[]{(byte)255,0}))throw new AssertionError("hybrid duplicate payload must use last value");
        byte[] boundary=new byte[4+65535];Arrays.fill(boundary,(byte)255);boundary[0]=10;boundary[1]=(byte)128;boundary[4]=0;boundary[boundary.length-1]=(byte)128;
        if(!Arrays.equals(hybrid(boundary).payload,Arrays.copyOfRange(boundary,4,boundary.length)))throw new AssertionError("65535B payload boundary");
        if(hybrid(new byte[0]).payload.length!=0)throw new AssertionError("empty hybrid message");
        if(EventOuterClass.Event.parseFrom(new byte[0]).getId()!=0||EventOuterClass.FixedEvent.parseFrom(new byte[0]).getId()!=0)throw new AssertionError("empty proto defaults");
        System.out.println("PROBE\thybrid_duplicate_last_wins\tPASS");
        System.out.println("PROBE\thybrid_duplicate_payload_last_wins\tPASS");
        System.out.println("PROBE\thybrid_65535B_payload_boundary\tPASS");
        System.out.println("PROBE\tempty_message_defaults\tPASS");
        return 17;
    }
    public static void main(String[] args)throws Exception {
        if(args.length!=1)throw new IllegalArgumentException("InteropCheck normalized.tsv");
        int fixtures=0,protoCount=0,hybridCount=0,variants=0;
        try(BufferedReader in=Files.newBufferedReader(Path.of(args[0]),StandardCharsets.UTF_8)) {
            String line;while((line=in.readLine())!=null){if(line.startsWith("case\t")||line.isEmpty())continue;
                String[] c=line.split("\t",-1);if(c.length!=15)throw new IllegalArgumentException("expected 15 TSV columns, got "+c.length);
                String name=c[0],codec=c[1],plan=c[2],type=c[3];Decoded expected=new Decoded();
                for(int i=0;i<9;i++){expected.scalar[i]=Long.parseUnsignedLong(c[4+i]);if(i>=5&&Long.compareUnsigned(expected.scalar[i],0xffffffffL)>0)throw new IllegalArgumentException("uint32 expected overflow");}
                expected.payload=Base64.getDecoder().decode(c[13]);byte[] wire=Base64.getDecoder().decode(c[14]);
                boolean isHybrid=codec.toLowerCase(Locale.ROOT).contains("hybrid")||type.equals("Hybrid");
                Decoded actual=isHybrid?hybrid(wire):protobuf(wire,type);equal(expected,actual,name+"/"+codec+"/"+plan);fixtures++;
                if(isHybrid){
                    hybridCount++;byte[] unknown={(byte)253,63,1,(byte)255,0,(byte)170,(byte)254,127,1,2,3,4,5,6,7,(byte)128,(byte)255,(byte)191,3,0,(byte)255,0,(byte)170};
                    equal(expected,hybrid(join(wire,unknown)),name+" unknown Hybrid extension");variants++;
                }else{
                    protoCount++;
                    // Unknown field #127, varint 77; Java must preserve it without changing known values.
                    byte[] unknown={(byte)248,7,77};byte[] withUnknown=join(wire,unknown);
                    if(fixed(type)){
                        EventOuterClass.FixedEvent parsed=EventOuterClass.FixedEvent.parseFrom(withUnknown);
                        if(!parsed.getUnknownFields().hasField(127)||!parsed.getUnknownFields().getField(127).getVarintList().equals(List.of(77L)))throw new AssertionError("unknown PB field not preserved");
                        equal(expected,fromFixed(parsed),name+" unknown fixed field");
                        equal(expected,fromFixed(EventOuterClass.FixedEvent.parseFrom(parsed.toByteArray())),name+" Java fixed reserialization");
                    }else{
                        EventOuterClass.Event parsed=EventOuterClass.Event.parseFrom(withUnknown);
                        if(!parsed.getUnknownFields().hasField(127)||!parsed.getUnknownFields().getField(127).getVarintList().equals(List.of(77L)))throw new AssertionError("unknown PB field not preserved");
                        equal(expected,fromEvent(parsed),name+" unknown varint field");
                        equal(expected,fromEvent(EventOuterClass.Event.parseFrom(parsed.toByteArray())),name+" Java varint reserialization");
                    }variants+=2;
                }
                System.out.println("FIXTURE\t"+name+"\t"+codec+"\t"+plan+"\tPASS");
            }
        }
        if(fixtures==0)throw new AssertionError("no fixtures supplied");
        int probes=standaloneProbes();
        System.out.println("SUMMARY\t{\"fixtures\":"+fixtures+",\"protobuf_fixtures\":"+protoCount+",\"hybrid_fixtures\":"+hybridCount+",\"derived_variant_checks\":"+variants+",\"standalone_probes\":"+probes+",\"status\":\"PASS\"}");
    }
}
