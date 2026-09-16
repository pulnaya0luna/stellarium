package org.qtproject.qt.android;

import android.app.Activity;
import android.util.Log;
import android.view.Display;
import android.view.Window;
import android.view.WindowManager;

/**
 * High-refresh-rate request for the Stellarium Cardboard port.
 *
 * WHY THIS CLASS EXISTS
 * Android does NOT give an app the highest refresh rate the panel supports. An app that
 * does nothing gets the display's DEFAULT mode, which is usually 60 Hz even on a 120 Hz
 * phone. The high-refresh modes have to be requested explicitly.
 *
 * That matters directly for VR comfort. Motion-to-photon latency can never be shorter than
 * one frame interval, so the refresh rate sets a hard floor on it:
 *
 *     60 Hz  -> 16.67 ms floor
 *     90 Hz  -> 11.11 ms floor
 *     120 Hz ->  8.33 ms floor
 *
 * Our comfort research puts the sickness threshold at roughly 20 ms, and the US Army study
 * found measurable sickness from 39 ms upward. At 60 Hz the frame interval alone eats 83% of
 * the 20 ms budget before any sensor, render, or GPU cost is added. At 120 Hz it eats 42%.
 * So requesting the fast mode is one of the cheapest comfort wins available.
 *
 * WHY JAVA
 * Qt 6.7.3 exposes no public API for display mode or refresh rate, and its own Android
 * template manifest does not request one either. This has to be done through the platform
 * API, which is why it lives here rather than in C++.
 *
 * WHY THIS RUNS ON THE UI THREAD
 * Qt for Android runs its own event loop on a Qt-owned thread; that is NOT the Android UI
 * thread. Window operations -- in particular Window.setAttributes() -- must happen on the UI
 * thread. Calling them from the Qt thread is what made the first version of this request
 * fail. Everything that touches the Window is therefore posted with runOnUiThread().
 *
 * WHY setPreferredDisplayModeId AND NOT setFrameRate
 * minSdk for this port is 23. WindowManager.LayoutParams.preferredDisplayModeId is available
 * from API 23, whereas Window.setFrameRate only exists from API 30. Using the older field
 * means the request works on every device the port can install on.
 */
public class StelCardboardDisplay {
    private static final String TAG = "StelCardboardDisplay";

    /**
     * Ask the window for the highest refresh rate the display supports.
     *
     * Runs asynchronously on the UI thread; the outcome is reported through Logcat under the
     * "StelCardboardDisplay" tag rather than through the return value, because the work has to
     * happen on a different thread than the caller. Safe to call more than once.
     */
    public static void requestHighestRefreshRate(final Activity activity) {
        if (activity == null) {
            Log.w(TAG, "activity is null; cannot request a display mode");
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                applyHighestRefreshRate(activity);
            }
        });
    }

    /** The actual work. Always runs on the Android UI thread. */
    private static void applyHighestRefreshRate(Activity activity) {
        try {
            Window window = activity.getWindow();
            if (window == null) {
                Log.w(TAG, "window is null; cannot request a display mode");
                return;
            }
            Display display = window.getWindowManager().getDefaultDisplay();
            if (display == null) {
                Log.w(TAG, "display is null; cannot request a display mode");
                return;
            }

            Display.Mode[] modes = display.getSupportedModes();
            if (modes == null || modes.length == 0) {
                Log.w(TAG, "getSupportedModes returned "
                        + (modes == null ? "null" : "empty") + "; keeping the default mode");
                return;
            }

            // Pick the mode with the highest refresh rate. Modes with equal rate are
            // equivalent for our purpose, so the first one wins.
            Display.Mode best = modes[0];
            for (Display.Mode mode : modes) {
                if (mode.getRefreshRate() > best.getRefreshRate()) {
                    best = mode;
                }
            }

            WindowManager.LayoutParams params = window.getAttributes();
            params.preferredDisplayModeId = best.getModeId();
            window.setAttributes(params);

            Log.i(TAG, "panel offers " + modes.length + " mode(s); requested mode id="
                    + best.getModeId() + " at " + best.getRefreshRate() + " Hz"
                    + " (was " + display.getRefreshRate() + " Hz)");
        } catch (Throwable t) {
            // A display-mode request must never take the app down. Worst case we keep the
            // default mode, which is exactly what would have happened without this call.
            Log.w(TAG, "display mode request failed", t);
        }
    }

    /**
     * Report the refresh rate currently in effect, for diagnostics.
     * Returns 0 if it cannot be determined.
     */
    public static float getCurrentRefreshRate(Activity activity) {
        if (activity == null) {
            return 0f;
        }
        try {
            Display display = activity.getWindowManager().getDefaultDisplay();
            if (display == null) {
                return 0f;
            }
            return display.getRefreshRate();
        } catch (Throwable t) {
            return 0f;
        }
    }
}
