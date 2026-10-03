import android.media.*;
import java.io.*;
import java.nio.*;
public class VenusEncode {
 public static void main(String[] args) throws Exception {
  int w=args.length>1?Integer.parseInt(args[1]):320;
  int h=args.length>2?Integer.parseInt(args[2]):240;
  int nframes=args.length>3?Integer.parseInt(args[3]):60,frames=0;
  MediaFormat f=MediaFormat.createVideoFormat("video/avc",w,h);
  f.setInteger(MediaFormat.KEY_COLOR_FORMAT,21);
  f.setInteger(MediaFormat.KEY_BIT_RATE,500000);
  f.setInteger(MediaFormat.KEY_FRAME_RATE,30);
  f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);
  MediaCodec c=MediaCodec.createByCodecName("c2.qti.avc.encoder");
  MediaMuxer mux=new MediaMuxer(args[0],MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
  int track=-1,sent=0; boolean eos=false;
  byte[] raw=new byte[w*h*3/2];
  java.util.Arrays.fill(raw,0,w*h,(byte)80);
  java.util.Arrays.fill(raw,w*h,raw.length,(byte)128);
  try {
   c.configure(f,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE); c.start();
   long end=System.nanoTime()+20_000_000_000L;
   MediaCodec.BufferInfo bi=new MediaCodec.BufferInfo();
   while(!eos && System.nanoTime()<end) {
    if(sent<=nframes) {
     int ix=c.dequeueInputBuffer(10000);
     if(ix>=0) {
      if(sent==nframes) c.queueInputBuffer(ix,0,0,sent*1000000L/30,MediaCodec.BUFFER_FLAG_END_OF_STREAM);
      else {
       for(int y=0;y<h;y++) java.util.Arrays.fill(raw,y*w,(y+1)*w,(byte)(16+(y+sent*7)%220));
       c.getInputBuffer(ix).put(raw);c.queueInputBuffer(ix,0,raw.length,sent*1000000L/30,0);
      }
      sent++;
     }
    }
    int ox=c.dequeueOutputBuffer(bi,10000);
    if(ox==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {track=mux.addTrack(c.getOutputFormat());mux.start();}
    if(ox>=0) {
     if(bi.size>0 && (bi.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)==0) {
      mux.writeSampleData(track,c.getOutputBuffer(ox),bi);frames++;
     }
     eos=(bi.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;c.releaseOutputBuffer(ox,false);
    }
   }
   System.out.println("ENCODER=c2.qti.avc.encoder RESULT frames="+frames+" EOS="+eos);
   if(!eos || frames!=nframes) throw new Exception("encode incomplete");
   c.stop();mux.stop();
  } finally {c.release();mux.release();}
 }
}
