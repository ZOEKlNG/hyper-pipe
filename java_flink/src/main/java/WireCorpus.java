import java.nio.*;
import java.nio.file.*;
import java.io.*;
import java.util.*;
import bench.EventOuterClass;

/** Fixed64 + Protobuf corpus exported by the C++ benchmark, identical bytes. */
public final class WireCorpus {
    public final byte[][] bytes;
    public final ByteBuffer[] heap;
    public final ByteBuffer[] direct;
    public final String codec;
    public final int payloadBytes;
    public WireCorpus(String dir, String codec, int payloadBytes, boolean allocateDirect) throws IOException {
        this.codec=codec; this.payloadBytes=payloadBytes;
        String prefix=codec.equals("protobuf")?"protobuf_varint_reuse":"fixed64_offset_trusted";
        ByteBuffer file=ByteBuffer.wrap(Files.readAllBytes(Path.of(dir,prefix+"_"+payloadBytes+"_random.bin"))).order(ByteOrder.LITTLE_ENDIAN);
        ArrayList<byte[]> list=new ArrayList<>();
        while(file.hasRemaining()) {
            if(file.remaining()<4) throw new IOException("short corpus prefix");
            int n=file.getInt(); if(n<0 || n>file.remaining()) throw new IOException("bad corpus size");
            byte[] a=new byte[n]; file.get(a); list.add(a);
        }
        bytes=list.toArray(new byte[0][]); heap=new ByteBuffer[bytes.length]; direct=new ByteBuffer[bytes.length];
        for(int i=0;i<bytes.length;i++) {
            heap[i]=ByteBuffer.wrap(bytes[i]).order(ByteOrder.LITTLE_ENDIAN);
            if(allocateDirect) { direct[i]=ByteBuffer.allocateDirect(bytes[i].length).order(ByteOrder.LITTLE_ENDIAN);direct[i].put(bytes[i]); }
            if(!codec.equals("protobuf")) validate(heap[i]);
        }
    }
    static void validate(ByteBuffer b) throws IOException {
        if(b.capacity()<64 || b.getShort(56)!=64 || b.getShort(58)!=1 || b.getInt(60)!=b.capacity()-64) throw new IOException("invalid fixed wire");
    }
    static long fixedSum(ByteBuffer b) {
        long s=b.getLong(0)+b.getLong(8)+b.getLong(16)+b.getLong(24)+b.getLong(32);
        s+=Integer.toUnsignedLong(b.getInt(40))+Integer.toUnsignedLong(b.getInt(44))+Integer.toUnsignedLong(b.getInt(48))+Integer.toUnsignedLong(b.getInt(52));
        int n=b.getInt(60), off=Short.toUnsignedInt(b.getShort(56));
        return s+n+(n==0?0:Byte.toUnsignedInt(b.get(off))+Byte.toUnsignedInt(b.get(off+n-1)));
    }
    static long protobufSum(EventOuterClass.Event m) {
        int n=m.getPayload().size();
        return m.getId()+m.getSeq()+m.getTimestamp()+m.getValue0()+m.getValue1()+Integer.toUnsignedLong(m.getProducer())+Integer.toUnsignedLong(m.getStream())+Integer.toUnsignedLong(m.getType())+Integer.toUnsignedLong(m.getFlags())+n+(n==0?0:Byte.toUnsignedInt(m.getPayload().byteAt(0))+Byte.toUnsignedInt(m.getPayload().byteAt(n-1)));
    }
    long sum(int i, String mode) throws IOException {
        if(mode.equals("protobuf")) return protobufSum(EventOuterClass.Event.parseFrom(bytes[i]));
        return fixedSum(mode.equals("direct")?direct[i]:heap[i]);
    }
}
