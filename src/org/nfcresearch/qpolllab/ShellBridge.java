package org.nfcresearch.qpolllab;
import android.os.*;
import android.content.pm.ApplicationInfo;
import java.io.*;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Shizuku service with fixed operations and bounded Binder messages. */
public final class ShellBridge extends Binder {
 private final int appUid; private final String apk;
 private final Object lock=new Object();private final StringBuilder logs=new StringBuilder();
 private volatile boolean active,cancelled;private volatile File stop;
 public ShellBridge()throws Exception {
  if(android.os.Process.myUid()!=0&&android.os.Process.myUid()!=2000)throw new SecurityException("Shell/root required");
  Object pm=Class.forName("android.app.ActivityThread").getMethod("getPackageManager").invoke(null);
  ApplicationInfo info=(ApplicationInfo)Class.forName("android.content.pm.IPackageManager").getMethod("getApplicationInfo",String.class,long.class,int.class).invoke(pm,"org.nfcresearch.qpolllab",0L,0);
  if(info==null)throw new IllegalStateException("App missing");appUid=info.uid;apk=info.sourceDir;
 }
 private void log(String line){synchronized(lock){logs.append(line).append('\n');if(logs.length()>50000)logs.delete(0,logs.indexOf("\n",logs.length()-40000)+1);}}
 private void cancel(){cancelled=true;File file=stop;if(file!=null)try{file.createNewFile();}catch(Exception ignored){}}
 @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
  if(code==16777115){cancel();new Thread(()->{long deadline=SystemClock.elapsedRealtime()+330000;while(active&&SystemClock.elapsedRealtime()<deadline)SystemClock.sleep(100);System.exit(0);},"nfc-destroy").start();return true;}
  if(code<1||code>3)return super.onTransact(code,data,reply,flags);
  if(Binder.getCallingUid()!=appUid)throw new SecurityException("Wrong caller");data.enforceInterface("org.nfcresearch.qpolllab.NfcSession");
  if(code==1){String mode=data.readString();int seconds=data.readInt();String json=data.readString();byte[] image=data.createByteArray();IBinder owner=data.readStrongBinder();
   if(mode==null||!mode.matches("inspect|standard|qtag|identity|refresh")||seconds<1||seconds>60||json==null||json.length()>8192||image==null||image.length>105984||owner==null)throw new IllegalArgumentException("Invalid NFC session");
   synchronized(lock){if(active)throw new IllegalStateException("另一会话尚未结束");active=true;cancelled=false;logs.setLength(0);}
   IBinder.DeathRecipient death=this::cancel;try{owner.linkToDeath(death,0);}catch(RemoteException e){active=false;throw e;}
   new Thread(()->{try{run(mode,seconds,json,image);}catch(Exception e){log("QP|ERROR|"+e);}finally{owner.unlinkToDeath(death,0);synchronized(lock){logs.append("QP|BRIDGE_END|后端会话结束\n");active=false;}}},"nfc-session").start();reply.writeNoException();return true;
  }
  if(code==2){synchronized(lock){reply.writeNoException();reply.writeString(logs.toString());logs.setLength(0);reply.writeInt(active?1:0);}return true;}
  cancel();reply.writeNoException();return true;
 }
 private static String quote(String s){return "'"+s.replace("'","'\\''")+"'";}
 private void run(String mode,int seconds,String json,byte[] bytes)throws Exception {
  final int uid=android.os.Process.myUid();
  final boolean qpoll=mode.equals("qtag")||mode.equals("identity")||mode.equals("refresh");
  log("QP|BRIDGE_STATE|UID="+uid+"；模式="+mode);
  if(qpoll&&uid!=0){log("QP|ERROR|Q-Poll 需要 Shizuku 以 Root 启动（当前 UID="+uid+"）");return;}
  File dir=new File("/data/local/tmp/qpoll-bridge-"+android.os.Process.myPid()+"-"+System.nanoTime());if(!dir.mkdir())throw new IOException("Cannot create session directory");
  if(new ProcessBuilder("chmod","700",dir.toString()).start().waitFor()!=0)throw new IOException("Cannot protect session directory");
  File mark=new File(dir,"stop"),job=new File(dir,"job.json"),image=new File(dir,"image.bin"),lock=new File(dir,"session.lock"),worker=new File(dir,"worker.sh");stop=mark;
  java.lang.Process child=null;
  try{
   if(cancelled)mark.createNewFile();JSONObject settings=new JSONObject(json);String hookDir=settings.optString("hookDir","");if(bytes.length>0)settings.put("imageFile",image.toString());
   try(FileOutputStream out=new FileOutputStream(image)){out.write(bytes);}try(FileOutputStream out=new FileOutputStream(job)){out.write(settings.toString().getBytes("UTF-8"));}
   if(uid==0){
    String args=mode+" "+seconds+" "+quote(mark.toString())+" "+quote(lock.toString())+(mode.equals("identity")||mode.equals("refresh")?" "+quote(job.toString()):"");
    String script="#!/system/bin/sh\nCLASSPATH="+quote(apk)+"\nexport CLASSPATH\nexec /system/bin/app_process /system/bin org.nfcresearch.qpolllab.QPollWorker "+args+"\n";
    write(worker,script);installHook(dir,hookDir,qpoll);
    if(new ProcessBuilder("chown","-R","2000:2000",dir.toString()).start().waitFor()!=0)throw new IOException("Cannot hand session directory to shell UID");
    String inner=(new File("/system/bin/runcon").exists()?"runcon u:r:shell:s0 ":"")+"sh "+quote(worker.toString());
    child=new ProcessBuilder("su","2000","-c",inner).redirectErrorStream(true).start();
   }else{
    java.util.ArrayList<String> command=new java.util.ArrayList<>();java.util.Collections.addAll(command,"/system/bin/app_process","/system/bin","org.nfcresearch.qpolllab.QPollWorker",mode,""+seconds,mark.toString(),lock.toString());if(mode.equals("identity")||mode.equals("refresh"))command.add(job.toString());
    ProcessBuilder builder=new ProcessBuilder(command).redirectErrorStream(true);builder.environment().put("CLASSPATH",apk);child=builder.start();
   }
   final java.lang.Process running=child;
   Thread deadline=new Thread(()->{try{if(!running.waitFor(320,TimeUnit.SECONDS)){mark.createNewFile();if(!running.waitFor(10,TimeUnit.SECONDS))running.destroy();}}catch(Exception ignored){}});deadline.setDaemon(true);deadline.start();
   try(BufferedReader in=new BufferedReader(new InputStreamReader(child.getInputStream(),"UTF-8"))){String line;while((line=in.readLine())!=null)log(line);}
   int exit=child.waitFor();if(exit!=0)log("QP|PROCESS_EXIT|"+exit);
  }finally{
   try{new ProcessBuilder("/system/bin/sh","-c","rm -f /data/vendor/nfc/qphook/enable").start().waitFor();}catch(Exception ignored){}
   mark.createNewFile();if(child!=null&&!child.waitFor(10,TimeUnit.SECONDS))child.destroy();stop=null;job.delete();image.delete();mark.delete();lock.delete();worker.delete();new File(dir,"hook.sh").delete();dir.delete();
  }
 }
 private static void write(File file,String text)throws IOException{try(FileOutputStream out=new FileOutputStream(file)){out.write(text.getBytes("UTF-8"));}file.setReadable(true,true);}
 private void installHook(File dir,String hookDir,boolean enabled)throws Exception{
  if(hookDir==null||hookDir.isEmpty()){log("QP|HOOK|未提供兼容层路径，跳过注入");return;}
  File script=new File(dir,"hook.sh");
  // A new file name per session: overwriting a .so that is already mapped in
  // the HAL can corrupt its code pages and crash the vendor process.
  String unique="libqpollhook-"+System.nanoTime()+".so";
  String dest="/data/vendor/nfc/qphook/"+unique;
  String text="#!/system/bin/sh\n"
   +"RES=/data/local/tmp/qpolllab-res\n"
   +"mkdir -p \"$RES\"\n"
   +"cp "+quote(new File(hookDir,"qpoll-inject").getPath())+" \"$RES/qpoll-inject\" || echo 'QP|HOOK|无法复制注入器'\n"
   +"cp "+quote(new File(hookDir,"libqpollhook.so").getPath())+" \"$RES/libqpollhook.so\" || echo 'QP|HOOK|无法复制兼容层'\n"
   +"chmod 0755 \"$RES/qpoll-inject\"\n"
   +"mkdir -p /data/vendor/nfc/qphook 2>/dev/null\n"
   +"cp \"$RES/libqpollhook.so\" "+quote(dest)+" || echo 'QP|HOOK|无法复制兼容层到 HAL 目录'\n"
   +"chown nfc:nfc "+quote(dest)+" 2>/dev/null\n"
   +"chmod 0755 "+quote(dest)+" 2>/dev/null\n"
   +"chcon u:object_r:vendor_file:s0 "+quote(dest)+" 2>/dev/null || echo 'QP|HOOK|chcon 失败，HAL 可能拒绝加载'\n"
   +"if \"$RES/qpoll-inject\" "+quote(dest)+"; then echo 'QP|HOOK|兼容层已注入 NFC HAL'; else echo 'QP|HOOK|兼容层注入失败'; fi\n"
   +"for f in /data/vendor/nfc/qphook/libqpollhook-*.so; do [ \"$f\" = "+quote(dest)+" ] || rm -f \"$f\"; done\n"
   +(enabled
     ?": > /data/vendor/nfc/qphook/enable && chown nfc:nfc /data/vendor/nfc/qphook/enable 2>/dev/null; chmod 0644 /data/vendor/nfc/qphook/enable; chcon u:object_r:vendor_file:s0 /data/vendor/nfc/qphook/enable 2>/dev/null; echo 'QP|HOOK|Q-Poll 发现已启用'\n"
     :"rm -f /data/vendor/nfc/qphook/enable; echo 'QP|HOOK|Q-Poll 发现未启用'\n");
  write(script,text);
  java.lang.Process process=new ProcessBuilder("/system/bin/sh",script.toString()).redirectErrorStream(true).start();
  try(BufferedReader in=new BufferedReader(new InputStreamReader(process.getInputStream(),"UTF-8"))){String line;while((line=in.readLine())!=null)log(line);}
  int code=process.waitFor();if(code!=0)log("QP|HOOK|注入脚本退出码="+code);
 }
}
