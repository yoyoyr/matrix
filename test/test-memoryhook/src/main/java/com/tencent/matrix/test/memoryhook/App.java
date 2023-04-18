package com.tencent.matrix.test.memoryhook;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Context;
import android.os.Process;
import android.os.SystemClock;

import com.tencent.matrix.AppActiveMatrixDelegate;
import com.tencent.matrix.Matrix;
import com.tencent.matrix.plugin.Plugin;
import com.tencent.matrix.plugin.PluginListener;
import com.tencent.matrix.report.Issue;
import com.tencent.matrix.trace.TracePlugin;
import com.tencent.matrix.trace.config.SharePluginInfo;
import com.tencent.matrix.trace.config.TraceConfig;
import com.tencent.matrix.trace.constants.Constants;
import com.tencent.matrix.util.MatrixLog;

import org.json.JSONException;

import java.util.List;

public class App extends Application {

    public static App app;

    public static long startTime;


    String TAG ="App";

    @Override
    protected void attachBaseContext(Context base) {
        startTime = SystemClock.elapsedRealtime();
        super.attachBaseContext(base);
    }

    @Override
    public void onCreate() {
        app = this;
        super.onCreate();

        TraceConfig config = new TraceConfig.Builder()
                .anrTracePath(getCacheDir().getPath() + "/trace.txt")
                .printTracePath(getCacheDir().getPath() + "/print_trace.txt")
                .enableSignalAnrTrace(true)
                .build();
        TracePlugin tracePlugin = new TracePlugin(config);

        Matrix matrix = new Matrix.Builder(this)
                .plugin(tracePlugin)
                .pluginListener(new PluginListener() {

                    @Override
                    public void onInit(Plugin plugin) {

                    }

                    @Override
                    public void onStart(Plugin plugin) {

                    }

                    @Override
                    public void onStop(Plugin plugin) {

                    }

                    @Override
                    public void onDestroy(Plugin plugin) {

                    }

                    @Override
                    public void onReportIssue(Issue issue) {
                        MatrixLog.i(TAG, "issue  = "+issue.toString());

                        try {
                            if(issue.getContent().get(SharePluginInfo.ISSUE_STACK_TYPE) == Constants.Type.SIGNAL_ANR){
                                ActivityManager am = (ActivityManager) App.app.getSystemService(Context.ACTIVITY_SERVICE);
                                List<ActivityManager.ProcessErrorStateInfo> procs = am.getProcessesInErrorState();
                                for (ActivityManager.ProcessErrorStateInfo proc : procs) {

                                    if (proc.uid != Process.myUid()
                                            && proc.condition == ActivityManager.ProcessErrorStateInfo.NOT_RESPONDING) {
                                        MatrixLog.i(TAG, "maybe received other apps ANR signal");
                                        break;
                                    }

                                    if (proc.pid != Process.myPid()) continue;

                                    if (proc.condition != ActivityManager.ProcessErrorStateInfo.NOT_RESPONDING) {
                                        continue;
                                    }

                                    MatrixLog.i(TAG, "error sate longMsg = %s", proc.longMsg);
                                }

                                String scene = AppActiveMatrixDelegate.INSTANCE.getVisibleScene();

                                MatrixLog.i(TAG, "sence = %s", scene);
                                MatrixLog.i(TAG, "start long =" + (SystemClock.elapsedRealtime() - App.startTime));

                            }
                        } catch (JSONException e) {
                            throw new RuntimeException(e);
                        }

                    }
                })
                .build();
        Matrix.init(matrix);

        tracePlugin.start();
    }
}
