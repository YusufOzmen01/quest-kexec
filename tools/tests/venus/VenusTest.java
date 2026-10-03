import android.media.*;
import java.nio.ByteBuffer;
public class VenusTest {
 public static void main(String[] args) throws Exception {
  if(args.length==0) {
   for(MediaCodecInfo i:new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos())
    if(i.isHardwareAccelerated()) System.out.println(i.getName()+" encoder="+i.isEncoder());
   return;
  }
  MediaExtractor ex=new MediaExtractor(); ex.setDataSource(args[0]);
  MediaFormat fmt=ex.getTrackFormat(0); ex.selectTrack(0);
  String name=args.length>1?args[1]:new MediaCodecList(MediaCodecList.ALL_CODECS).findDecoderForFormat(fmt);
  System.out.println("DECODER="+name+" FORMAT="+fmt);
  MediaCodec c=MediaCodec.createByCodecName(name);
  int frames=0; boolean inputDone=false, outputDone=false;
  try {
   c.configure(fmt,null,null,0); c.start();
   long deadline=System.nanoTime()+20_000_000_000L;
   MediaCodec.BufferInfo bi=new MediaCodec.BufferInfo();
   while(!outputDone && System.nanoTime()<deadline) {
    if(!inputDone) {
     int ix=c.dequeueInputBuffer(10000);
     if(ix>=0) {
      ByteBuffer b=c.getInputBuffer(ix); int n=ex.readSampleData(b,0);
      if(n<0) {c.queueInputBuffer(ix,0,0,0,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputDone=true;}
      else {c.queueInputBuffer(ix,0,n,ex.getSampleTime(),0);ex.advance();}
     }
    }
    int ox=c.dequeueOutputBuffer(bi,10000);
    if(ox>=0) {
     if(bi.size>0) frames++;
     outputDone=(bi.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
     c.releaseOutputBuffer(ox,false);
    } else if(ox==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) System.out.println("OUTPUT="+c.getOutputFormat());
   }
   System.out.println("RESULT frames="+frames+" EOS="+outputDone);
   if(!outputDone || frames==0) throw new Exception("decode incomplete");
   c.stop();
  } finally { c.release(); ex.release(); }
 }
}
