package com.test.webgpu;

import android.app.Activity;
import android.os.Bundle;

public class MainActivity extends Activity {
    static { System.loadLibrary("test_native"); }

    private static native void testCppWrappers();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }
}
