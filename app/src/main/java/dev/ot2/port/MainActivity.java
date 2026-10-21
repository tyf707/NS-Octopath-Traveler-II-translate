package dev.ot2.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import java.lang.ref.WeakReference;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.graphics.Color;
import android.view.View;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.InputDevice;
import android.hardware.input.InputManager;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.EditText;
import android.text.InputFilter;
import android.util.AtomicFile;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity implements InputManager.InputDeviceListener {
    static { System.loadLibrary("ot2host"); }
    private static native String runNativeProbe(String directory,String privateDirectory,boolean inspectModules);
    private static native void bootNative(String directory,String logPath,long programId);
    private static native void setNativeSurface(Surface surface);
    private static native long getNativeFrameCount();
    private static native double[] getNativeFrameStats();
    private static native int setNativeTargetFps(int target);
    private static native void setNativeAspectMode(int mode);
    private static native void setNativeEngineKind(int kind);
    private static native void setNativePatchList(String list);
    private static native void setNativePresentInterval(int value);
    private Surface frameRateSurface;
    private int hintedFrameRate;
    private int applyTargetFps(int target){
        int applied=setNativeTargetFps(target);
        synchronized(surfaceLock){
            Surface current=activeHolder==null?null:activeHolder.getSurface();
            int hint=new File(getFilesDir(),"shaders/no-surface-frame-rate").exists()?0:target;
            if(current!=null&&current.isValid()&&(current!=frameRateSurface||hintedFrameRate!=hint)){
                try{
                    current.setFrameRate(hint,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
                    frameRateSurface=current;hintedFrameRate=hint;
                }catch(IllegalArgumentException|IllegalStateException unavailable){
                    // Surface replacement can race the compositor. Retry on
                    // the next HUD refresh; the native pacer remains active.
                    frameRateSurface=null;
                }
            }
        }
        return applied;
    }
    private PerformanceHud performanceHud;
    private static native void setNativeFocus(int state);
    private static native int takeNativeSaveNotice();
    private static native String[] getNativeTextInput();
    private static native boolean completeNativeTextInput(long id,boolean accepted,String text);
    interface TextInputBridge {String[] pending();boolean complete(long id,boolean accepted,String text);}
    TextInputBridge textInputBridge=new TextInputBridge(){
        @Override public String[] pending(){return getNativeTextInput();}
        @Override public boolean complete(long id,boolean accepted,String text){return completeNativeTextInput(id,accepted,text);}
    };
    private final Handler noticeHandler=new Handler(Looper.getMainLooper());
    private AlertDialog saveDialog;
    private AlertDialog textDialog;
    private final Runnable noticePoll=new Runnable(){@Override public void run(){
        if(!resumed || isDestroyed())return;
        refreshAutomationInput();
        acceptSaveNotice(takeNativeSaveNotice());
        acceptTextInput(textInputBridge.pending());
        noticeHandler.postDelayed(this,500);
    }};
    void acceptTextInput(String[] request){
        if(request==null){if(textDialog!=null)textDialog.dismiss();return;}
        if(textDialog!=null || saveDialog!=null || !resumed || isFinishing())return;
        long id=Long.parseLong(request[0]);int minimum=Integer.parseInt(request[5]),maximum=Integer.parseInt(request[6]);
        EditText text=new EditText(this);text.setSingleLine(true);text.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maximum)});
        text.setText(request[4]);text.setSelection(text.length());text.setHint(request[3]);
        textDialog=new AlertDialog.Builder(this).setTitle(request[1].isEmpty()?"输入文字":request[1]).setMessage(request[2])
            .setView(text).setPositiveButton("确定",null).setNegativeButton("取消",(dialog,which)->textInputBridge.complete(id,false,""))
            .setOnCancelListener(dialog->textInputBridge.complete(id,false,"")).create();
        textDialog.setOnDismissListener(dialog->textDialog=null);
        textDialog.setOnKeyListener((dialog,code,event)->{
            if(code!=KeyEvent.KEYCODE_BUTTON_A&&code!=KeyEvent.KEYCODE_BUTTON_B)return false;
            if(event.getAction()==KeyEvent.ACTION_UP){if(code==KeyEvent.KEYCODE_BUTTON_B){textInputBridge.complete(id,false,"");dialog.dismiss();}else textDialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();}return true;
        });
        textDialog.show();textDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view->{
            String value=text.getText().toString();if(value.length()<minimum){text.setError("至少输入 "+minimum+" 个字符");return;}
            if(textInputBridge.complete(id,true,value))textDialog.dismiss();
        });
        text.requestFocus();textDialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
    }
    // Main-thread state. Session keeps unacknowledged notices through Activity
    // replacement; no native callback owns an Activity or waits for the UI.
    void acceptSaveNotice(int flags){
        Session.pendingSaveNotice|=flags;
        int pending=Session.pendingSaveNotice;
        if(pending==0 || !resumed || isFinishing() || isDestroyed())return;
        String message=(pending&4)!=0
            ?"存档写入后未能确认已安全保存。请检查设备剩余空间并再次保存；在确认成功前，不要关闭游戏。"
            :(pending&1)!=0
            ?"存档目前只能读取，本次游玩的新进度无法保存。请检查设备剩余空间，重新启动游戏后再试。已保存的进度未被修改。"
            :"本次保存失败，之前已保存的进度未被修改。请检查设备剩余空间后再次保存。";
        if((pending&5)==5)message+="\n当前存档也只能读取，无法再次保存。";
        if(saveDialog!=null){saveDialog.setMessage(message);return;}
        saveDialog=new AlertDialog.Builder(this).setTitle("存档提示").setMessage(message)
            .setCancelable(false).setPositiveButton("知道了",(dialog,which)->Session.pendingSaveNotice=0).create();
        saveDialog.setOnDismissListener(dialog->saveDialog=null);
        saveDialog.setOnKeyListener((dialog,code,event)->{
            if(code!=KeyEvent.KEYCODE_BUTTON_A)return false;
            if(event.getAction()==KeyEvent.ACTION_UP)saveDialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
            return true;
        });
        saveDialog.show();
    }
    private boolean resumed;
    private boolean bootRequested(){return !getIntent().getBooleanExtra("probe_only",false);}
    private static native void setNativeInputTrace(boolean enabled);
    private boolean inputTrace,automationInput;
    private boolean acceptsInput(int deviceId){return !automationInput||deviceId==android.view.KeyCharacterMap.VIRTUAL_KEYBOARD;}
    private void refreshAutomationInput(){
        boolean next=new File(getFilesDir(),"automation-input").exists();
        if(next!=automationInput){releaseInput();automationInput=next;if(inputTrace)android.util.Log.i("OT2Input","automation="+next);}
    }
    private static final Object surfaceLock=new Object();
    private static SurfaceHolder activeHolder;
    private TextView status;
    private Button retry;
    private boolean started;
    private static native void setNativePad(long buttons,int lx,int ly,int rx,int ry);
    private static native void clearNativePad();
    private static final ActivitySession activitySession=new ActivitySession(MainActivity::setNativeFocus,MainActivity::clearNativePad);
    private static native void setNativeTouch(int action,int id,float x,float y,int width,int height);
    private static native void cancelNativeTouch();
    private GameInputView gameInput;
    private long virtualButtons;
    private int virtualLx,virtualLy,virtualRx,virtualRy;
    private long keyButtons,axisButtons;
    private int lx,ly,rx,ry,controllerId=-1;
    private void updateButtons(){if(resumed&&activitySession.owns(this))setNativePad(keyButtons|axisButtons|virtualButtons,lx!=0?lx:virtualLx,ly!=0?ly:virtualLy,rx!=0?rx:virtualRx,ry!=0?ry:virtualRy);}
    private void releaseInput(){if(inputTrace)android.util.Log.i("OT2Input","clear device="+controllerId);keyButtons=axisButtons=0;lx=ly=rx=ry=0;virtualButtons=0;virtualLx=virtualLy=virtualRx=virtualRy=0;if(gameInput!=null)gameInput.cancel();if(activitySession.owns(this))clearNativePad();}
    private static long buttonForKey(int code) {
        switch(code) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_Z: return 1;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_X: return 2;
            case KeyEvent.KEYCODE_BUTTON_X: return 4;
            case KeyEvent.KEYCODE_BUTTON_Y: return 8;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 1L<<4;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 1L<<5;
            case KeyEvent.KEYCODE_BUTTON_L1: return 1L<<6;
            case KeyEvent.KEYCODE_BUTTON_R1: return 1L<<7;
            case KeyEvent.KEYCODE_BUTTON_L2: return 1L<<8;
            case KeyEvent.KEYCODE_BUTTON_R2: return 1L<<9;
            case KeyEvent.KEYCODE_BUTTON_START: return 1L<<10;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 1L<<11;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 1L<<12;
            case KeyEvent.KEYCODE_DPAD_UP: return 1L<<13;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 1L<<14;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 1L<<15;
            default:return 0;
        }
    }
    @Override public boolean onKeyDown(int code,KeyEvent event) {long bit=buttonForKey(code);if(bit!=0&&!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-key device="+event.getDeviceId()+" code="+code);return true;}if(inputTrace)android.util.Log.i("OT2Input","down code="+code+" mapped="+bit+" device="+event.getDeviceId()+" source="+event.getSource()+" repeat="+event.getRepeatCount()+" time="+event.getEventTime());if(bit!=0){controllerId=event.getDeviceId();keyButtons|=bit;updateButtons();return true;}return super.onKeyDown(code,event);}
    @Override public boolean onKeyUp(int code,KeyEvent event) {long bit=buttonForKey(code);if(bit!=0&&!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-key device="+event.getDeviceId()+" code="+code);return true;}if(inputTrace)android.util.Log.i("OT2Input","up code="+code+" mapped="+bit+" device="+event.getDeviceId()+" source="+event.getSource()+" repeat="+event.getRepeatCount()+" time="+event.getEventTime());if(bit!=0){keyButtons&=~bit;updateButtons();return true;}return super.onKeyUp(code,event);}
    private static float axis(MotionEvent event,int axis) {
        InputDevice device=event.getDevice();
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());
        float value=event.getAxisValue(axis),flat=range==null?0.12f:Math.max(0.12f,range.getFlat());
        return Math.abs(value)<=flat?0:Math.copySign(Math.min(1,(Math.abs(value)-flat)/(1-flat)),value);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if((event.getSource()&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK && event.getAction()==MotionEvent.ACTION_MOVE) {
            if(!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-axis device="+event.getDeviceId()+" x="+event.getAxisValue(MotionEvent.AXIS_X)+" y="+event.getAxisValue(MotionEvent.AXIS_Y));return true;}
            long oldButtons=axisButtons;int oldLx=lx,oldLy=ly,oldRx=rx,oldRy=ry;
            controllerId=event.getDeviceId();
            lx=Math.round(axis(event,MotionEvent.AXIS_X)*32767);ly=Math.round(-axis(event,MotionEvent.AXIS_Y)*32767);
            InputDevice device=event.getDevice();
            boolean zr=device!=null && device.getMotionRange(MotionEvent.AXIS_Z,event.getSource())!=null;
            rx=Math.round(axis(event,zr?MotionEvent.AXIS_Z:MotionEvent.AXIS_RX)*32767);
            ry=Math.round(-axis(event,zr?MotionEvent.AXIS_RZ:MotionEvent.AXIS_RY)*32767);
            float hx=event.getAxisValue(MotionEvent.AXIS_HAT_X),hy=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            axisButtons=(hx<-.5f?1L<<12:0)|(hy<-.5f?1L<<13:0)|(hx>.5f?1L<<14:0)|(hy>.5f?1L<<15:0);
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),event.getAxisValue(MotionEvent.AXIS_BRAKE))>.5f)axisButtons|=1L<<8;
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),event.getAxisValue(MotionEvent.AXIS_GAS))>.5f)axisButtons|=1L<<9;
            if(inputTrace&&(oldButtons!=axisButtons||oldLx!=lx||oldLy!=ly||oldRx!=rx||oldRy!=ry))android.util.Log.i("OT2Input","axes device="+controllerId+" buttons="+axisButtons+" lx="+lx+" ly="+ly+" rx="+rx+" ry="+ry+" time="+event.getEventTime());
            updateButtons();return true;
        }
        return super.onGenericMotionEvent(event);
    }
    private void refreshController(){
        if(gameInput==null)return;
        // 0=自动（接手柄时隐藏）1=始终隐藏 2=始终显示；shaders/hide-controls 仍可强制隐藏。
        final int mode=setting("buttons",0);
        boolean hide;
        if(mode==1)hide=true;
        else if(mode==2)hide=false;
        else{
            hide=GameInputView.controllerPresent();
            if(!hide){try{hide=new File(getFilesDir(),"shaders/hide-controls").exists();}catch(RuntimeException ignored){}}
        }
        if(gameInput.setControllerConnected(hide))releaseInput();
    }
    private final Runnable controllerPoll=new Runnable(){@Override public void run(){
        if(!resumed||isDestroyed())return;
        refreshController();
        noticeHandler.postDelayed(this,2000);
    }};
    @Override public void onInputDeviceAdded(int id) {refreshController();}
    @Override public void onInputDeviceChanged(int id) {if(id==controllerId)releaseInput();refreshController();}
    @Override public void onInputDeviceRemoved(int id) {if(id==controllerId){releaseInput();controllerId=-1;}refreshController();}
    @Override protected void onStart(){super.onStart();activitySession.start(this);}
    @Override protected void onResume(){super.onResume();resumed=true;refreshAutomationInput();getSystemService(InputManager.class).registerInputDeviceListener(this,null);activitySession.resume(this,hasWindowFocus());refreshController();noticeHandler.post(controllerPoll);noticeHandler.post(noticePoll);if(performanceHud!=null)performanceHud.start();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(resumed){activitySession.focus(this,focused);if(!focused)releaseInput();}}
    @Override protected void onPause(){resumed=false;noticeHandler.removeCallbacks(controllerPoll);noticeHandler.removeCallbacks(noticePoll);if(performanceHud!=null)performanceHud.stop();if(saveDialog!=null)saveDialog.dismiss();if(textDialog!=null)textDialog.dismiss();getSystemService(InputManager.class).unregisterInputDeviceListener(this);activitySession.pause(this);releaseInput();super.onPause();}
    @Override protected void onStop(){activitySession.stop(this);super.onStop();}


    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        inputTrace=getIntent().getBooleanExtra("input_trace",false);setNativeInputTrace(inputTrace);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(Color.rgb(20,25,32));
        int pad = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad,pad,pad,pad);
        layout.setOnApplyWindowInsetsListener((view,insets) -> {
            android.graphics.Insets bars=insets.getInsets(android.view.WindowInsets.Type.systemBars());
            view.setPadding(pad+bars.left,pad+bars.top,pad+bars.right,pad+bars.bottom);
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("NS-translate-Android");
        title.setTextSize(24); title.setTextColor(Color.rgb(236,216,171));
        layout.addView(title);
        TextView note = new TextView(this);
        note.setText("本地游戏 · ARM64 + Vulkan\n无手柄时显示屏幕按键，连接后自动隐藏。\n");
        note.setTextSize(16); note.setTextColor(Color.LTGRAY); layout.addView(note);
        retry = new Button(this); retry.setText("重新运行验证");
        retry.setOnClickListener(v -> probe()); layout.addView(retry);
        status = new TextView(this); status.setTextSize(15); status.setTextColor(Color.WHITE);
        status.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this); scroll.addView(status);
        layout.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        if(bootRequested()) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            FrameLayout root=new FrameLayout(this);
            SurfaceView surface=new SurfaceView(this);
            root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
            layout.removeAllViews();scroll.removeView(status);
            layout.setBackgroundColor(Color.TRANSPARENT);status.setTextSize(12);
            layout.addView(status,new LinearLayout.LayoutParams(-1,-2));
            root.addView(layout,new FrameLayout.LayoutParams(-1,-2));
            gameInput=new GameInputView(this,new GameInputView.Sink(){
                @Override public void pad(long buttons,int x,int y,int z,int w){padCombo(buttons);virtualButtons=buttons;virtualLx=x;virtualLy=y;virtualRx=z;virtualRy=w;updateButtons();}
                @Override public void touch(int action,int id,float x,float y,int w,int h){if(resumed&&hasWindowFocus()&&activitySession.owns(MainActivity.this))setNativeTouch(action,id,x,y,w,h);}
                @Override public void cancelTouch(){if(activitySession.owns(MainActivity.this))cancelNativeTouch();}
            });
            root.addView(gameInput,new FrameLayout.LayoutParams(-1,-1));
            installSettings(root);
            applySettings();
            performanceHud=new PerformanceHud(this,MainActivity::getNativeFrameStats,this::applyTargetFps);
            FrameLayout.LayoutParams hudParams=new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.TOP|android.view.Gravity.START);
            int margin=Math.round(12*getResources().getDisplayMetrics().density);
            hudParams.setMargins(margin,margin,margin,margin);root.addView(performanceHud,hudParams);
            performanceHud.setOnClickListener(v->performanceHud.setVisibility(View.GONE));
            root.postDelayed(()->{if(performanceHud!=null&&!isDestroyed())performanceHud.setVisibility(View.GONE);},6000);
            performanceHud.setOnApplyWindowInsetsListener((view,insets)->{
                android.graphics.Insets safe=insets.getInsets(android.view.WindowInsets.Type.displayCutout());
                FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)view.getLayoutParams();
                p.setMargins(margin+safe.left,margin+safe.top,margin+safe.right,margin+safe.bottom);view.setLayoutParams(p);return insets;
            });
            surface.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder holder) {
                    synchronized(surfaceLock){frameRateSurface=null;activeHolder=holder;setNativeSurface(holder.getSurface());}
                    if(!started){started=true;probe();}
                }
                @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) {}
                @Override public void surfaceDestroyed(SurfaceHolder holder) {synchronized(surfaceLock){if(activeHolder==holder){frameRateSurface=null;activeHolder=null;setNativeSurface(null);}}}
            });
            setContentView(root);
            root.post(()->{
                getWindow().getInsetsController().hide(android.view.WindowInsets.Type.systemBars());
                getWindow().getInsetsController().setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            });
            root.postDelayed(new Runnable(){@Override public void run(){if(isDestroyed())return;if(getNativeFrameCount()>4){status.setVisibility(View.GONE);return;}root.postDelayed(this,500);}},500);
        } else {setContentView(layout);layout.requestApplyInsets();probe();}
    }

    // The native game owns process-wide state and outlives Activity recreation.
    // Only application Context and weak UI references may cross bootNative().
    private static final class Session {
        enum Phase { IDLE, VALIDATING, RUNNING, ENDED, FAILED }
        private static final ExecutorService worker=Executors.newSingleThreadExecutor();
        private static Phase phase=Phase.IDLE;
        private static boolean gameClaimed,bootRequested;
        private static int pendingSaveNotice;
        private static WeakReference<MainActivity> observer=new WeakReference<>(null);
        private static String message="正在校验并启动游戏…";
        private static void publish(String text){synchronized(Session.class){message=text;notifyObserver();}}
        private static void notifyObserver(){
            MainActivity activity=observer.get();if(activity==null)return;
            String text=message;boolean retryAllowed=!gameClaimed && phase!=Phase.VALIDATING;
            // The posted callback is short-lived; the native worker never owns
            // an Activity while the game runs indefinitely.
            activity.runOnUiThread(()->{if(!activity.isDestroyed()){activity.status.setText(text);activity.retry.setEnabled(retryAllowed);}});
        }
        static synchronized void detach(MainActivity activity){if(observer.get()==activity)observer.clear();}
        static synchronized void request(MainActivity activity,boolean boot){
            observer=new WeakReference<>(activity);
            if(gameClaimed){notifyObserver();return;}
            bootRequested|=boot;
            if(phase==Phase.VALIDATING){notifyObserver();return;}
            phase=Phase.VALIDATING;message="正在校验并启动游戏…";notifyObserver();
            Context context=activity.getApplicationContext();
            String title=activity.getIntent().getStringExtra("title_id");
            worker.execute(()->run(context,title));
        }
        private static void run(Context context,String title){
            try {
                long started=android.os.SystemClock.elapsedRealtime();
                GameProfile profile=GameProfile.select(context,title);
                // 引擎适配器：决定帧率/存档等引擎相关策略，并告知 native。
                EngineHooks engine=EngineHooks.of(EngineHooks.detect(profile.gameRoot,profile.manifest));
                EngineHooks.install(engine);
                setNativeEngineKind(engine.kind().nativeId());
                // 按标题的补丁：由 game-build.json 的 patches 数组声明；缺失时 native 用内置默认。
                String patches=joinPatches(profile.manifest.optJSONArray("patches"));
                File directory=new File(profile.gameRoot,"ExeFS");
                String validation=profile.validateModules();
                long verified=android.os.SystemClock.elapsedRealtime();
                boolean inspect; synchronized(Session.class){inspect=!bootRequested;}
                // Boot maps, decompresses and links the modules itself. The
                // explicit probe retains full inspection without doing that
                // same expensive work twice on every normal launch.
                JSONObject report=new JSONObject(runNativeProbe(validation.isEmpty()?directory.getAbsolutePath():"",profile.privateRoot.getAbsolutePath(),inspect));
                report.put("title_id",profile.titleId);
                if(patches!=null) setNativePatchList(patches);
                report.put("engine",engine.kind().label());
                if(patches!=null) report.put("patches",patches);
                report.put("engine_frame_rate_patch",engine.supportsFrameRatePatch());
                report.put("engine_frame_rate_patch_kind",engine.frameRatePatchKind());
                report.put("module_validation_ms",verified-started);
                report.put("probe_ms",android.os.SystemClock.elapsedRealtime()-verified);
                report.put("model",Build.MODEL);report.put("soc",Build.SOC_MODEL);
                report.put("android_api",Build.VERSION.SDK_INT);
                report.put("module_validation",validation.isEmpty()?"sha256_verified":validation);
                report.put("timestamp_ms",System.currentTimeMillis());
                AtomicFile output=new AtomicFile(new File(profile.privateRoot,"probe.json"));
                FileOutputStream stream=output.startWrite();
                try{stream.write(report.toString(2).getBytes(StandardCharsets.UTF_8));output.finishWrite(stream);}
                catch(Exception error){output.failWrite(stream);throw error;}
                boolean valid=validation.isEmpty() && report.has("execution_bridge")
                    && report.optBoolean("save_atomic_publish") && report.optBoolean("synthetic_arm64_fixture") && report.optBoolean("nvn_texture_transfers")
                    && report.optBoolean("nvn_descriptor_reuse") && report.optBoolean("nvn_resource_lifecycle") && report.optBoolean("nvn_texture_cpu_coherence") && report.optBoolean("nvn_gpu_deswizzle")
                    && report.optBoolean("aaudio_disconnect_reopen_injected") && (!inspect||report.has("loader"));
                boolean startGame;
                synchronized(Session.class){
                    startGame=bootRequested && valid;
                    phase=startGame?Phase.RUNNING:Phase.IDLE;
                    if(startGame)gameClaimed=true;
                }
                if(startGame){
                    publish("正在启动游戏…");
                    bootNative(directory.getAbsolutePath(),new File(profile.privateRoot,"boot.log").getAbsolutePath(),Long.parseUnsignedLong(profile.titleId,16));
                    synchronized(Session.class){phase=Phase.ENDED;}
                    publish("游戏运行已结束。");
                }else{
                    publish(render(report));
                }
            }catch(Exception error){
                synchronized(Session.class){phase=Phase.FAILED;}
                publish("验证失败："+error.getMessage());
            }
        }
    }
    private void probe(){Session.request(this,bootRequested());}

    private static String render(JSONObject r) {
        StringBuilder text=new StringBuilder();
        text.append(r.optString("model")).append(" · ").append(r.optString("soc")).append('\n');
        text.append("内存页：").append(r.optLong("page_size")).append(" bytes\n\n");
        if(r.has("compatibility_error"))return text.append(r.optString("compatibility_error")).append("\n\n本次未启动游戏。设备检查结果已保存。").toString();
        text.append("ARM64 合成指令测试：").append(r.optBoolean("synthetic_arm64_fixture")?"通过":"失败").append('\n');
        JSONObject gpu=r.optJSONObject("vulkan");
        if(gpu!=null) text.append(gpu.optString("device")).append(" · Vulkan ").append(gpu.optString("api"))
                .append("\nGPU 清屏并回读：通过\nBC 纹理：").append(gpu.optBoolean("texture_bc")?"原生支持":"软件解码").append('\n');
        else text.append("Vulkan：").append(r.optString("vulkan_error")).append('\n');
        text.append("\n游戏模块校验：").append(r.optString("module_validation")).append('\n');
        JSONObject loader=r.optJSONObject("loader");
        if(loader!=null) text.append(loader.optJSONArray("modules").length()).append(" 个模块已装载\n重定位：").append(loader.optLong("relocations")).append(" 处\n");
        if(loader!=null && loader.optJSONObject("sdk_leaf")!=null) text.append("原版 SDK 独立函数已原生执行：")
                .append(loader.optJSONObject("sdk_leaf").optLong("returned_hz")).append(" Hz\n");
        if(r.has("loader_error")) text.append("装载错误：").append(r.optString("loader_error")).append('\n');
        text.append("\nCPU 执行边界：").append(r.has("execution_bridge")?"已验证":"未通过");
        text.append("\n待验证：游戏画面、音频、输入、存档。\n\n");
        text.append("报告保存于应用内部 files/probe.json");
        return text.toString();
    }
    // ---- 隐藏设置面板：连点屏幕顶部中间 5 次打开 ----
    private static final String SETTINGS_PREFS="ot2-settings";
    private View settingsPanel;
    private long settingsFirstTap;
    private int settingsTapCount;
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    private int setting(String key,int fallback){try{return getSharedPreferences(SETTINGS_PREFS,0).getInt(key,fallback);}catch(RuntimeException error){return fallback;}}
    private void putSetting(String key,int value){try{getSharedPreferences(SETTINGS_PREFS,0).edit().putInt(key,value).apply();}catch(RuntimeException ignored){}}
    /** "patches": ["settime-host"] → "settime-host"；数组存在但为空时返回 ""（显式不启用）。 */
    private static String joinPatches(org.json.JSONArray patches){
        if(patches==null)return null;
        StringBuilder list=new StringBuilder();
        for(int i=0;i<patches.length();i++){
            String id=patches.optString(i,"");
            if(id.isEmpty())continue;
            if(list.length()>0)list.append(',');
            list.append(id);
        }
        return list.toString();
    }
    private void applySettings(){
        setNativeAspectMode(setting("aspect",2));
        // 用户没手动选过帧率时，用引擎适配器给的默认值（Unity/UE4 默认 60Hz 上限）。
        final int unset=Integer.MIN_VALUE;
        final int saved=setting("interval",unset);
        setNativePresentInterval(saved==unset?EngineHooks.current().defaultPresentInterval():saved);
        refreshController();
    }
    private static final long PAD_PLUS=1L<<10,PAD_MINUS=1L<<11;   // 虚拟手柄 + / −
    private static final long PAD_COMBO_WINDOW_MS=700;
    private boolean padComboLatched,padPlusHeld,padMinusHeld;
    private long padPlusAt,padMinusAt;
    private void toggleSettings(){
        if(settingsPanel==null)return;
        settingsPanel.setVisibility(settingsPanel.getVisibility()==View.VISIBLE?View.GONE:View.VISIBLE);
    }
    private void padCombo(long buttons){
        final boolean plus=(buttons&PAD_PLUS)!=0,minus=(buttons&PAD_MINUS)!=0;
        final long now=android.os.SystemClock.uptimeMillis();
        if(minus&&!padMinusHeld){
            padMinusHeld=true;padMinusAt=now;
            // 两指同时按，或 0.7 秒内先后按下，都算组合键。
            if(plus||now-padPlusAt<=PAD_COMBO_WINDOW_MS){padComboLatched=true;toggleSettings();}
        }
        if(!minus)padMinusHeld=false;
        if(plus&&!padPlusHeld){
            padPlusHeld=true;padPlusAt=now;
            if(minus||now-padMinusAt<=PAD_COMBO_WINDOW_MS){padComboLatched=true;toggleSettings();}
        }
        if(!plus)padPlusHeld=false;
        if(!minus&&!plus)padComboLatched=false;
    }
    private void registerSettingsTap(){
        final long now=android.os.SystemClock.uptimeMillis();
        if(settingsTapCount==0||now-settingsFirstTap>2500){settingsFirstTap=now;settingsTapCount=1;return;}
        if(++settingsTapCount>=5){settingsTapCount=0;if(settingsPanel!=null)settingsPanel.setVisibility(settingsPanel.getVisibility()==View.VISIBLE?View.GONE:View.VISIBLE);}
    }
    private void installSettings(FrameLayout root){
        View hotspot=new View(this);
        hotspot.setOnClickListener(v->registerSettingsTap());
        root.addView(hotspot,new FrameLayout.LayoutParams(dp(240),dp(90),Gravity.TOP|Gravity.CENTER_HORIZONTAL));

        LinearLayout panel=new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xF0101820);
        final int pad=dp(22);panel.setPadding(pad,pad,pad,pad);
        panel.setClickable(true);panel.setFocusable(true);

        LinearLayout titleRow=new LinearLayout(this);titleRow.setOrientation(LinearLayout.HORIZONTAL);titleRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView title=new TextView(this);title.setText("画面与性能");title.setTextSize(20);title.setTextColor(0xFFFFFFFF);
        title.setLayoutParams(new LinearLayout.LayoutParams(0,-2,1));
        titleRow.addView(title);
        Button close=new Button(this);close.setText("关闭");
        close.setOnClickListener(v->{View parent=panel.getParent() instanceof View?(View)panel.getParent():panel;parent.setVisibility(View.GONE);});
        titleRow.addView(close,new LinearLayout.LayoutParams(-2,-2));
        panel.addView(titleRow);

        panel.addView(sectionLabel("画面填充"));
        RadioGroup aspect=new RadioGroup(this);
        addRadio(aspect,"拉伸填满（无黑边）",2);
        addRadio(aspect,"保持比例（左右黑边）",0);
        addRadio(aspect,"裁切填满（上下裁切）",1);
        panel.addView(aspect);
        checkGroup(aspect,setting("aspect",2));
        aspect.setOnCheckedChangeListener((group,id)->{int value=(Integer)group.findViewById(id).getTag();putSetting("aspect",value);setNativeAspectMode(value);});

        panel.addView(sectionLabel("帧率上限"));
        RadioGroup fps=new RadioGroup(this);
        addRadio(fps,"60fps（默认，过场更快）",1);
        addRadio(fps,"跟随游戏 30fps",-1);
        panel.addView(fps);
        checkGroup(fps,setting("interval",1));
        fps.setOnCheckedChangeListener((group,id)->{int value=(Integer)group.findViewById(id).getTag();putSetting("interval",value);setNativePresentInterval(value);});

        panel.addView(sectionLabel("屏幕虚拟按键"));
        RadioGroup buttons=new RadioGroup(this);
        addRadio(buttons,"自动（接手柄时隐藏）",0);
        addRadio(buttons,"始终显示",2);
        addRadio(buttons,"始终隐藏",1);
        panel.addView(buttons);
        checkGroup(buttons,setting("buttons",0));
        TextView hint=new TextView(this);hint.setText("提示：同时按屏幕上的「−」和「+」可随时打开/关闭本面板");
        hint.setTextColor(0xFF7F93A8);hint.setTextSize(12);hint.setPadding(0,dp(12),0,0);
        panel.addView(hint);
        buttons.setOnCheckedChangeListener((group,id)->{int value=(Integer)group.findViewById(id).getTag();putSetting("buttons",value);refreshController();});


        ScrollView scroller=new ScrollView(this);
        scroller.setBackgroundColor(0xF0101820);
        scroller.setVisibility(View.GONE);
        scroller.addView(panel);
        FrameLayout.LayoutParams panelParams=new FrameLayout.LayoutParams(dp(600),dp(246),Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        panelParams.topMargin=dp(10);
        root.addView(scroller,panelParams);
        settingsPanel=scroller;
    }
    private TextView sectionLabel(String text){
        TextView label=new TextView(this);label.setText(text);label.setTextColor(0xFF9FB3C8);label.setTextSize(13);
        label.setPadding(0,dp(9),0,0);return label;
    }
    private void addRadio(RadioGroup group,String text,int value){
        RadioButton button=new RadioButton(this);
        button.setId(View.generateViewId());button.setText(text);button.setTextColor(0xFFFFFFFF);button.setTextSize(14);button.setPadding(0,0,0,0);button.setTag(value);
        group.addView(button);
    }
    private void checkGroup(RadioGroup group,int value){
        for(int i=0;i<group.getChildCount();i++){
            View child=group.getChildAt(i);
            if(child instanceof RadioButton && ((Integer)child.getTag())==value){group.check(child.getId());return;}
        }
    }
    @Override public void onDestroy() { noticeHandler.removeCallbacks(controllerPoll);noticeHandler.removeCallbacks(noticePoll);if(saveDialog!=null)saveDialog.dismiss();Session.detach(this);activitySession.destroy(this);super.onDestroy(); }
}
