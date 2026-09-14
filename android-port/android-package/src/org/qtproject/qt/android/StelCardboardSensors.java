package org.qtproject.qt.android;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;

/**
 * Latest-sample head-orientation provider for the Stellarium Cardboard port.
 *
 * WHY THIS CLASS EXISTS
 * Qt's QRotationSensor reports Euler angles only (x/y/z in degrees), and Euler angles
 * have a singularity at pitch = +/-90 degrees. That is exactly the pose a planetarium
 * user adopts when tilting their head back to look at the zenith, so Euler angles are
 * unusable here: the sky would whip around at the worst possible moment.
 *
 * Android's SensorManager exposes the same fused orientation as a proper unit quaternion
 * via TYPE_GAME_ROTATION_VECTOR, which has no such singularity. This class registers a
 * listener and keeps only the most recent sample.
 *
 * WHY ONLY THE MOST RECENT SAMPLE
 * Head tracking wants the freshest orientation, not a backlog. If frames are slow we must
 * render the newest pose available, never replay stale events in a burst -- replaying a
 * queue produces exactly the visible tracking jump that causes cybersickness.
 *
 * WHY THE "GAME" VARIANT
 * TYPE_GAME_ROTATION_VECTOR fuses gyroscope + accelerometer and deliberately excludes the
 * magnetometer. Magnetic heading drifts unpredictably near metal, which would rotate the
 * sky while the user holds still. Absolute heading is meaningless inside a headset.
 */
public class StelCardboardSensors implements SensorEventListener {
    private static SensorManager sensorManager;
    private static Sensor rotationSensor;

    // Latest sample, guarded by this class's monitor. [x, y, z, w]
    private static final float[] latest = new float[] {0f, 0f, 0f, 1f};
    private static boolean haveSample = false;
    private static final Object lock = new Object();

    /** Start delivering orientation samples. Safe to call more than once. */
    public static boolean start(Context context) {
        if (sensorManager != null) {
            return true;
        }
        try {
            sensorManager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
            if (sensorManager == null) {
                return false;
            }
            rotationSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
            if (rotationSensor == null) {
                sensorManager = null;
                return false;
            }
            StelCardboardSensors listener = new StelCardboardSensors();
            // SENSOR_DELAY_GAME (~50 Hz) is ample for head tracking and much kinder to the
            // battery than SENSOR_DELAY_FASTEST.
            return sensorManager.registerListener(listener, rotationSensor, SensorManager.SENSOR_DELAY_GAME);
        } catch (Throwable t) {
            sensorManager = null;
            rotationSensor = null;
            return false;
        }
    }

    /** Stop delivering samples and release the sensor. */
    public static void stop() {
        if (sensorManager != null) {
            try {
                sensorManager.unregisterListener(new StelCardboardSensors());
            } catch (Throwable ignored) {
            }
            sensorManager = null;
            rotationSensor = null;
        }
        synchronized (lock) {
            haveSample = false;
        }
    }

    /** Human-readable sensor name, or null when unavailable. */
    public static String getSensorName() {
        return rotationSensor == null ? null : rotationSensor.getName();
    }

    /** True once at least one real sample has arrived. */
    public static boolean hasSample() {
        synchronized (lock) {
            return haveSample;
        }
    }

    /**
     * Copy the latest orientation into out[0..3] as a unit quaternion (x, y, z, w).
     * Returns true when a sample was available.
     */
    public static boolean getLatestQuaternion(float[] out) {
        synchronized (lock) {
            if (!haveSample || out == null || out.length < 4) {
                return false;
            }
            System.arraycopy(latest, 0, out, 0, 4);
            return true;
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event == null || event.sensor == null) {
            return;
        }
        if (event.sensor.getType() != Sensor.TYPE_GAME_ROTATION_VECTOR) {
            return;
        }
        if (event.values == null || event.values.length < 4) {
            return;
        }
        synchronized (lock) {
            // Keep only the newest sample; overwrite unconditionally.
            latest[0] = event.values[0];
            latest[1] = event.values[1];
            latest[2] = event.values[2];
            latest[3] = event.values[3];
            haveSample = true;
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        // Not acted on: the fused rotation vector is either usable or it is not, and
        // reacting to accuracy changes would add view movement the user did not cause.
    }
}
