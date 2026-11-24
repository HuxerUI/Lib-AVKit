package org.huxerui.lib.avkit.preview;

import android.app.Application;

import org.libpag.PAG;

public final class MainApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        // Initialize libpag with the application context before its native decoder accesses the cache.
        PAG.SDKVersion();
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
