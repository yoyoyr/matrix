package com.tencent.matrix.test.memoryhook;

import android.app.Application;

import com.tencent.matrix.Matrix;
import com.tencent.matrix.plugin.Plugin;
import com.tencent.matrix.plugin.PluginListener;
import com.tencent.matrix.report.Issue;
import com.tencent.matrix.trace.TracePlugin;
import com.tencent.matrix.trace.config.TraceConfig;

public class App extends Application {

    @Override
    public void onCreate() {
        super.onCreate();
        Matrix matrix = new Matrix.Builder(this).build();
        Matrix.init(matrix);
        TraceConfig config = new TraceConfig.Builder()
                .anrTracePath(getCacheDir().getPath()+"/trace.txt")
                .printTracePath(getCacheDir().getPath()+"/print_trace.txt")
                .enableSignalAnrTrace(true)
                .build();
        TracePlugin tracePlugin = new TracePlugin(config);
        tracePlugin.init(this, new PluginListener() {

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

            }
        });
        tracePlugin.start();
    }
}
