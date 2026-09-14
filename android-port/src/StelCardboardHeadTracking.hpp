/*
 * Stellarium Cardboard port — head tracking.
 *
 * Reads the device orientation from Android's fused Game Rotation Vector sensor and
 * exposes it as a quaternion the renderer can consume.
 *
 * WHY THIS USES JNI INSTEAD OF QtSensors
 * --------------------------------------
 * Qt's QRotationSensor (QtSensors) is the obvious choice and it is NOT used here, for a
 * concrete reason: QRotationReading exposes only Euler angles x(), y(), z() -- there is
 * no quaternion API. Euler angles have a singularity (gimbal lock) at pitch = +/-90
 * degrees, and that is precisely where a planetarium user looks when they tilt their head
 * back to view the zenith. A gimbal-lock singularity would show up as the sky whipping
 * around exactly when the user looks straight up, which is both broken and nauseating.
 *
 * Android's SensorManager gives us TYPE_GAME_ROTATION_VECTOR as a true unit quaternion,
 * so we call it directly through JNI. Stellarium already uses this exact JNI pattern in
 * src/main.cpp for the storage permission, so it is consistent with the codebase.
 *
 * Comfort decisions (see notes/VR-COMFORT-RESEARCH.md for the evidence):
 *
 *  - TYPE_GAME_ROTATION_VECTOR, NOT TYPE_ROTATION_VECTOR. The "game" variant fuses
 *    gyroscope + accelerometer and deliberately EXCLUDES the magnetometer. Magnetic
 *    heading drifts unpredictably near metal and would rotate the sky while the user
 *    holds still -- a direct cybersickness trigger. Absolute heading is meaningless
 *    inside a headset anyway.
 *
 *  - No hand-rolled complementary/Madgwick filter. The platform already fuses these
 *    sensors well; integrating raw gyro output by hand drifts by degrees within minutes.
 *
 *  - Orientation is extrapolated forward by the expected motion-to-photon latency before
 *    it reaches the renderer. A US Army study found ~19 ms sub-threshold for sickness and
 *    28% of subjects withdrew at 254 ms; consumer HMDs run 21-42 ms raw and 2-13 ms
 *    effective once prediction is applied. Prediction is the single most important
 *    comfort lever available.
 *
 *  - Samples are timestamped against a monotonic clock and integrated with the real
 *    interval, never an assumed frame period, so a hitch cannot corrupt the prediction.
 */

#ifndef STELCARDBOARDHEADTRACKING_HPP
#define STELCARDBOARDHEADTRACKING_HPP

#include <QObject>
#include <QQuaternion>
#include <QElapsedTimer>
#include <QTimer>

//! Fused-orientation head tracker for Cardboard-style VR.
//! Produces a quaternion rotating the world into the device's frame.
class StelCardboardHeadTracking : public QObject
{
	Q_OBJECT

public:
	explicit StelCardboardHeadTracking(QObject* parent = nullptr);
	~StelCardboardHeadTracking() override;

	//! True when a suitable sensor was found and is delivering data.
	bool isActive() const { return active; }

	//! Last error/status string, for the diagnostics overlay.
	const QString& getStatus() const { return status; }

	//! Name of the sensor actually in use (diagnostics).
	const QString& getSensorName() const { return sensorName; }

	//! Start/stop listening. Safe to call repeatedly.
	void start();
	void stop();

	//! Orientation for the current frame, already extrapolated to display time.
	//! Identity when no data has arrived yet, so callers never null-check.
	QQuaternion getPredictedOrientation() const;

	//! Measured interval between the last two samples, seconds. 0 until we have two.
	double getSampleInterval() const { return lastSampleInterval; }

	//! Measured age of the newest sample, seconds (diagnostics).
	double getSampleAge() const;

	//! Prediction horizon currently applied, seconds.
	double getPredictionTime() const { return predictionTime; }
	void setPredictionTime(double seconds);

	//! Recentre: treat the device's current orientation as "looking forward".
	//! Comfort-relevant: lets the user start from a comfortable posture without the view
	//! lurching, and avoids an initial reorientation that reads as forced motion.
	void recenter();

	//! Poll the sensor once. Called internally on a timer; exposed so tests can drive it.
	void pollSensor();

signals:
	//! Emitted when sensor availability changes, so the UI can show a warning.
	void activeChanged(bool active);

private:
	bool attachSensor();
	void detachSensor();
	void processQuaternion(float x, float y, float z, float w);

	// Android sensor plumbing (JNI objects kept as opaque pointers to avoid pulling
	// JNI headers into this header).
	void* sensorManager = nullptr;   //!< jobject SensorManager
	void* rotationSensor = nullptr;  //!< jobject Sensor
	void* sensorEventQueue = nullptr;//!< jobject SensorEventQueue
	int sensorType = 0;

	QTimer pollTimer;

	QQuaternion rawOrientation;       //!< newest fused orientation, world->device
	QQuaternion referenceOrientation; //!< orientation captured at the last recenter()

	QElapsedTimer sampleClock;        //!< monotonic clock, stamped at each sample
	qint64 lastSampleNs = 0;          //!< previous sample's clock value
	double lastSampleInterval = 0.0;  //!< seconds between the last two samples

	QQuaternion lastDelta;            //!< last observed angular delta, for prediction
	bool haveDelta = false;

	double predictionTime = 0.020;    //!< 20 ms: at the measured sickness threshold

	bool active = false;
	bool haveReading = false;
	bool androidReady = false;
	QString status;
	QString sensorName;
};

#endif // STELCARDBOARDHEADTRACKING_HPP
