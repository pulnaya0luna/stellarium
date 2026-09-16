/*
 * Stellarium Cardboard port — head tracking implementation.
 *
 * See StelCardboardHeadTracking.hpp for why this reads Android's sensor API through a
 * small Java helper instead of QtSensors, and notes/VR-COMFORT-RESEARCH.md for the
 * comfort evidence behind each decision.
 */

#include "StelCardboardHeadTracking.hpp"
#include "StelCardboardComfortMath.hpp"

#include <QDebug>
#include <QtMath>

#ifdef Q_OS_ANDROID
# include <QJniEnvironment>
# include <QJniObject>
#endif

// Poll faster than any plausible display refresh. The sensor delivers at its own rate;
// polling more often simply picks up the newest available sample, which is exactly what
// head tracking wants. We deliberately never queue backlogged samples: a stale sample
// rendered late is worse than a fresh one rendered now.
static constexpr int POLL_INTERVAL_MS = 2;

static const char* SENSOR_HELPER_CLASS = "org/qtproject/qt/android/StelCardboardSensors";

StelCardboardHeadTracking::StelCardboardHeadTracking(QObject* parent)
	: QObject(parent)
{
	pollTimer.setInterval(POLL_INTERVAL_MS);
	pollTimer.setTimerType(Qt::PreciseTimer);
	connect(&pollTimer, &QTimer::timeout, this, &StelCardboardHeadTracking::pollSensor);
}

StelCardboardHeadTracking::~StelCardboardHeadTracking()
{
	stop();
}

void StelCardboardHeadTracking::start()
{
	if (active) return;

	sampleClock.start();

	if (!attachSensor())
	{
		active = false;
		emit activeChanged(false);
		return;
	}

	active = true;
	status = QStringLiteral("ok");
	pollTimer.start();
	emit activeChanged(true);
}

void StelCardboardHeadTracking::stop()
{
	pollTimer.stop();
	detachSensor();
	if (active)
	{
		active = false;
		emit activeChanged(false);
	}
}

bool StelCardboardHeadTracking::attachSensor()
{
#ifdef Q_OS_ANDROID
	// The Java helper holds the SensorManager and keeps only the newest sample.
	// Passing the application context lets it resolve SENSOR_SERVICE itself.
	QJniObject context = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
	                                                        "()Landroid/app/Activity;");

	if (!context.isValid())
	{
		status = QStringLiteral("no Android activity");
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(SENSOR_HELPER_CLASS, "start",
	                                                           "(Landroid/content/Context;)Z", context.object());

	if (!ok)
	{
		status = QStringLiteral("no GAME_ROTATION_VECTOR sensor on this device");
		return false;
	}

	const QString name = QJniObject::callStaticObjectMethod(SENSOR_HELPER_CLASS, "getSensorName",
	                                                        "()Ljava/lang/String;")
	                             .toString();
	sensorName         = name.isEmpty() ? QStringLiteral("(unnamed)") : name;

	androidReady = true;
	status       = QStringLiteral("ok");
	return true;
#else
	status = QStringLiteral("head tracking is Android-only");
	return false;
#endif
}

void StelCardboardHeadTracking::detachSensor()
{
#ifdef Q_OS_ANDROID
	if (androidReady)
	{
		QJniObject::callStaticMethod<void>(SENSOR_HELPER_CLASS, "stop", "()V");
	}
#endif
	androidReady = false;
}

void StelCardboardHeadTracking::pollSensor()
{
#ifdef Q_OS_ANDROID
	if (!androidReady) return;

	QJniEnvironment env;
	if (!env.isValid()) return;

	// Reuse a scratch array rather than allocating one per poll at 500 Hz.
	static jfloatArray scratch = nullptr;
	if (!scratch)
	{
		scratch = env->NewFloatArray(4);
		if (!scratch) return;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(SENSOR_HELPER_CLASS, "getLatestQuaternion", "([F)Z",
	                                                           scratch);

	if (!ok)
	{
		// No sample yet: normal during the first few milliseconds after registration.
		if (!haveReading) status = QStringLiteral("waiting for first sample");
		return;
	}

	float buf[4] = {0.f, 0.f, 0.f, 1.f};
	env->GetFloatArrayRegion(scratch, 0, 4, buf);

	processQuaternion(buf[0], buf[1], buf[2], buf[3]);
#endif
}

void StelCardboardHeadTracking::processQuaternion(float x, float y, float z, float w)
{
	const QQuaternion q(x, y, z, w);

	// Guard against a degenerate reading: some drivers emit an all-zero event before the
	// first real one. A bad quaternion fed into the view would snap the sky somewhere
	// arbitrary, which is both broken and nauseating.
	const float norm = std::sqrt(q.scalar() * q.scalar() + q.x() * q.x() + q.y() * q.y() + q.z() * q.z());
	if (norm < 0.5f || !std::isfinite(norm))
	{
		status = QStringLiteral("degenerate quaternion; ignoring sample");
		return;
	}

	// Normalise: the prediction maths assumes a unit quaternion.
	const QQuaternion unit = q / norm;

	const qint64 nowNs = sampleClock.nsecsElapsed();
	if (lastSampleNs != 0)
	{
		const double dt = double(nowNs - lastSampleNs) / 1e9;
		// Reject implausible intervals. A long gap (app backgrounded, or a stall) must not
		// be fed into the prediction or the view would fling on resume -- exactly the kind
		// of unexpected motion that causes sickness.
		if (StelCardboard::isUsableSampleInterval(dt))
		{
			lastSampleInterval = dt;
			lastDelta          = unit * rawOrientation.conjugated();
			haveDelta          = true;
		}
	}
	lastSampleNs = nowNs;

	rawOrientation = unit;
	haveReading    = true;
	status         = QStringLiteral("ok");
}

QQuaternion StelCardboardHeadTracking::getPredictedOrientation() const
{
	if (!haveReading) return QQuaternion();

	// The maths lives in StelCardboardComfortMath.hpp so the unit tests exercise exactly
	// this code path rather than a copy of it.
	return StelCardboard::predictedOrientation(rawOrientation, referenceOrientation, lastDelta, haveDelta,
	                                           predictionTime, lastSampleInterval);
}

double StelCardboardHeadTracking::getSampleAge() const
{
	if (!haveReading) return 0.0;
	return double(sampleClock.nsecsElapsed() - lastSampleNs) / 1e9;
}

void StelCardboardHeadTracking::setPredictionTime(double seconds)
{
	// Clamped in the shared maths so the safe window is defined in one place and is
	// covered by the unit tests.
	predictionTime = StelCardboard::clampPredictionTime(seconds);
}

void StelCardboardHeadTracking::setMeasuredLatency(double seconds)
{
	if (seconds <= 0.0) return;
	// Exponential moving average: smooth enough to read, responsive enough to notice a
	// regression. Deliberately not a history buffer to keep this cheap per frame.
	constexpr double ALPHA = 0.1;
	measuredLatency        = (measuredLatency <= 0.0) ? seconds : (1.0 - ALPHA) * measuredLatency + ALPHA * seconds;
}

void StelCardboardHeadTracking::recenter()
{
	referenceOrientation = rawOrientation;
	haveDelta            = false; // the old delta is meaningless in the new frame
}
