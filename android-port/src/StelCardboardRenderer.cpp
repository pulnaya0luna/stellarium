/*
 * Stellarium Cardboard port — VR presentation module implementation.
 * See StelCardboardRenderer.hpp for the design and comfort rationale, and
 * notes/VR-COMFORT-RESEARCH.md for the evidence behind each decision.
 */

#include "StelCardboardRenderer.hpp"
#include "StelCardboardHeadTracking.hpp"
#include "StelCardboardViewportEffect.hpp"

#include <StelApp.hpp>
#include <StelCore.hpp>
#include <StelMainView.hpp>
#include <StelModule.hpp>
#include <StelMovementMgr.hpp>
#include <StelProjector.hpp>

#include <cmath>
#include <QDebug>
#include <QSettings>
#include <QtMath>

#ifdef Q_OS_ANDROID
# include <QJniObject>
#endif

StelCardboardRenderer::StelCardboardRenderer()
{
	setObjectName(QStringLiteral("StelCardboardRenderer"));
}

StelCardboardRenderer::~StelCardboardRenderer()
{
	delete headTracking;
	// The effect is owned by StelApp once installed; do not delete it here.
}

void StelCardboardRenderer::init()
{
	// Head tracking starts here but never blocks rendering: if the device has no usable
	// rotation-vector sensor the tracker stays inactive, the status string says so, and
	// the sky still renders normally. A missing sensor must degrade to "no head tracking",
	// never to a dead app.
	headTracking = new StelCardboardHeadTracking(this);
	headTracking->start();

	// The display's DEFAULT mode is usually 60 Hz even on a 120 Hz panel: Android does not
	// hand an app the fast mode unless it asks. Requesting it is one of the cheapest comfort
	// wins available, because the refresh rate sets a hard floor on motion-to-photon latency
	// (16.67 ms at 60 Hz vs 8.33 ms at 120 Hz) and our sickness threshold is around 20 ms.
	// Qt exposes no API for this, so it goes through the Java helper.
	requestHighestRefreshRate();

	QSettings* conf = StelApp::getInstance().getSettings();
	if (conf)
	{
		stereoEnabled    = conf->value("cardboard/stereo", true).toBool();
		ipdMm            = qBound(50.0, conf->value("cardboard/ipd_mm", 64.0).toDouble(), 75.0);
		vignetteStrength = qBound(0.0f, conf->value("cardboard/vignette", 0.0f).toFloat(), 1.0f);
		snapTurnAngle    = conf->value("cardboard/snap_turn_deg", 30.0).toDouble();
		vrFov            = qBound(60.0, conf->value("cardboard/fov", 95.0).toDouble(), 120.0);
	}

	qInfo() << "[Cardboard] head tracking:" << headTracking->getStatus()
		<< "| sensor:" << headTracking->getSensorName() << "| active:" << headTracking->isActive();
	qInfo() << "[Cardboard] stereo:" << stereoEnabled << "| ipd:" << ipdMm << "mm | fov:" << vrFov << "deg"
		<< "| vignette:" << vignetteStrength << "| snap turn:" << snapTurnAngle << "deg";
}

double StelCardboardRenderer::getCallOrder(StelModuleActionName actionName) const
{
	// Runs last: by this point every other module has drawn the sky into the offscreen
	// buffer, and the viewport effect owns the final composite to screen.
	if (actionName == StelModule::ActionDraw) return 1000.0;
	return 0.0;
}

void StelCardboardRenderer::installEffect()
{
	if (effectInstalled || !stereoEnabled) return;

	StelApp& app = StelApp::getInstance();
	if (!app.getCore()) return;

	// Install the stereo compositor. This replaces the render buffer and viewport effect,
	// so it must happen after upstream's own setViewportEffect() call during init -- which
	// is why it is done lazily on the first frame rather than from init().
	app.setViewportEffect(QStringLiteral("cardboardStereo"));
	effectInstalled = (app.getViewportEffect() == QStringLiteral("cardboardStereo"));

	if (!effectInstalled)
	{
		qWarning() << "[Cardboard] failed to install the stereo viewport effect;"
			   << "falling back to mono rendering.";
		return;
	}

	// Fixed FOV for the whole session. A Cardboard lens shows roughly 90-100 degrees, so
	// the default 60-degree desktop FOV would look badly zoomed through the lenses.
	// Deliberately set once and never modulated: a pumping FOV is a discomfort source and
	// would also invalidate the lens distortion mapping. zoomTo with a zero duration is
	// the instant form.
	if (StelMovementMgr* mm = app.getCore()->getMovementMgr())
	{
		mm->setInitFov(vrFov);
		mm->zoomTo(vrFov, 0.f);
	}

	qInfo() << "[Cardboard] stereo viewport effect installed; screen" << screenSize.width() << "x"
		<< screenSize.height() << "| fov" << vrFov << "deg";
}

void StelCardboardRenderer::removeEffect()
{
	if (!effectInstalled) return;
	StelApp::getInstance().setViewportEffect(QStringLiteral("none"));
	effectInstalled = false;
}

void StelCardboardRenderer::setStereoEnabled(bool b)
{
	if (stereoEnabled == b) return;
	stereoEnabled = b;
	if (b)
		installEffect();
	else
		removeEffect();
}

void StelCardboardRenderer::setInterPupillaryDistance(double mm)
{
	ipdMm = qBound(50.0, mm, 75.0);
}

void StelCardboardRenderer::setVignetteStrength(float s)
{
	vignetteStrength = qBound(0.0f, s, 1.0f);
	// The effect reads its settings at construction and StelApp does not expose the
	// instance, so persist the value: a later install (or a restart) picks it up. Kept
	// honest rather than pretending it applies live.
	StelApp::immediateSave("cardboard/vignette", vignetteStrength);
}

QString StelCardboardRenderer::getStatusText() const
{
	if (!headTracking) return QStringLiteral("cardboard: not initialised");

	QString s = QStringLiteral("cardboard %1 | hmd %2 | ipd %3mm")
	                    .arg(effectInstalled ? QStringLiteral("stereo") : QStringLiteral("mono"))
	                    .arg(headTracking->isActive() ? QStringLiteral("on") : QStringLiteral("OFF"))
	                    .arg(ipdMm, 0, 'f', 1);

	if (headTracking->isActive())
	{
		s += QStringLiteral(" | pred %1ms | age %2ms")
		             .arg(headTracking->getPredictionTime() * 1000.0, 0, 'f', 1)
		             .arg(headTracking->getSampleAge() * 1000.0, 0, 'f', 1);
	}
	else
	{
		s += QStringLiteral(" | %1").arg(headTracking->getStatus());
	}
	return s;
}

void StelCardboardRenderer::draw(StelCore* core)
{
	if (!core) return;

	// Track the surface size so viewport maths stays correct across rotation.
	const QSize sz = StelMainView::getInstance().size();
	if (sz != screenSize) screenSize = sz;

	// Lazy install on the first frame. Upstream calls setViewportEffect() with the
	// configured value during StelApp::init(), AFTER modules are registered, so installing
	// from init() would be silently overwritten. Doing it here is order-independent.
	if (stereoEnabled && !effectInstalled) installEffect();

	// Drive the view from the head tracker.
	//
	// This is the piece that makes head tracking actually DO something: without it the
	// tracker runs and reports sensible values while the sky never moves.
	//
	// The orientation is applied as an incremental rotation of the view direction rather
	// than as an absolute one, because Stellarium's MountMode can be AltAzimuthal,
	// Equatorial, Galactic or Supergalactic -- an absolute mapping would only be correct
	// in one of them. Working in increments is correct in all of them, and it is also
	// what keeps the motion smooth: we apply the CHANGE in orientation since the last
	// frame, so a dropped frame simply means a slightly larger increment rather than a
	// jump.
	applyHeadOrientation(core);
}

void StelCardboardRenderer::requestHighestRefreshRate()
{
#ifdef Q_OS_ANDROID
	// Ask Android for the fastest display mode. Without this an app gets the display's
	// DEFAULT mode, which is usually 60 Hz even on a 120 Hz panel.
	//
	// This matters because the refresh rate is a hard floor on motion-to-photon latency:
	// 16.67 ms at 60 Hz, 8.33 ms at 120 Hz. With a ~20 ms sickness threshold, the 60 Hz
	// interval alone would consume 83% of the budget before any sensor or render cost.
	// The request is posted to the Android UI thread, so it completes asynchronously; the
	// outcome is reported by the Java side under the "StelCardboardDisplay" log tag. Here we
	// only confirm the call was made and record the rate in effect at this moment.
	QJniObject activity = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
	                                                         "()Landroid/app/Activity;");
	if (!activity.isValid())
	{
		qWarning() << "[Cardboard] no Android activity; cannot request a display mode";
		return;
	}

	QJniObject::callStaticMethod<void>("org/qtproject/qt/android/StelCardboardDisplay", "requestHighestRefreshRate",
	                                   "(Landroid/app/Activity;)V", activity.object());

	const jfloat cur = QJniObject::callStaticMethod<jfloat>("org/qtproject/qt/android/StelCardboardDisplay",
	                                                        "getCurrentRefreshRate", "(Landroid/app/Activity;)F",
	                                                        activity.object());
	qInfo().nospace() << "[Cardboard] requested the highest display mode; currently " << cur
			  << " Hz (outcome reported under the StelCardboardDisplay log tag)";
#endif
}

void StelCardboardRenderer::applyHeadOrientation(StelCore* core)
{
	if (!headTracking || !headTracking->isActive()) return;

	StelMovementMgr* mm = core->getMovementMgr();
	if (!mm) return;

	// One-shot probe: confirms the view-driving path is reached at all, and reports the
	// first orientation the sensor actually delivered.
	static bool probed = false;
	if (!probed)
	{
		probed               = true;
		const QQuaternion q0 = headTracking->getPredictedOrientation();
		qInfo().nospace() << "[Cardboard] view driver reached; first orientation q=(" << q0.x() << "," << q0.y()
				  << "," << q0.z() << "," << q0.scalar() << ") active=" << headTracking->isActive();
	}

	// The tracker already applies the recentring reference and the forward prediction.
	const QQuaternion q = headTracking->getPredictedOrientation();

	// First usable sample: adopt it as the baseline without moving the view. Otherwise the
	// very first frame would apply the whole accumulated orientation as one enormous
	// rotation, which is both disorienting and exactly the kind of unexpected motion the
	// comfort rules exist to avoid.
	if (!haveLastOrientation)
	{
		// Recentre too: the device boots at an arbitrary pose, and treating that pose as
		// "forward" is what lets the user hold the phone however is comfortable in the
		// viewer. Without this the sky is offset by whatever orientation the device
		// happened to start in.
		headTracking->recenter();
		lastOrientation     = headTracking->getPredictedOrientation();
		haveLastOrientation = true;
		return;
	}

	// Increment since the previous frame, in the device's own frame.
	const QQuaternion delta = q * lastOrientation.conjugated();
	lastOrientation         = q;

	// Convert to a rotation of the view direction. The device's +Y axis is "up" on the
	// screen, so yaw is rotation about the device's Y axis and pitch about its X axis.
	// Sign conventions are chosen so that turning the head right moves the sky left, as a
	// real window would.
	const float yawRad   = qDegreesToRadians(2.0f * std::atan2(delta.y(), delta.scalar()));
	const float pitchRad = qDegreesToRadians(2.0f * std::atan2(delta.x(), delta.scalar()));

	// Ignore sub-pixel noise. A stationary user must see a perfectly still sky: any
	// jitter here would read as the world trembling, which is both wrong and unpleasant.
	const double yawDeg   = qRadiansToDegrees(double(yawRad));
	const double pitchDeg = qRadiansToDegrees(double(pitchRad));
	// Low-rate diagnostic so head tracking can be verified from a log without a
	// headset. Cheap enough to leave in: one line every ~2 seconds.
	static int diagCounter = 0;
	if (++diagCounter % 60 == 0)
	{
		qInfo().nospace() << "[Cardboard] hmd yaw=" << yawDeg << " pitch=" << pitchDeg << " q=(" << q.x() << ","
				  << q.y() << "," << q.z() << "," << q.scalar() << ")"
				  << " age=" << headTracking->getSampleAge() * 1000.0 << "ms";
	}
	constexpr double DEADZONE_DEG = 0.005;
	if (std::abs(yawDeg) < DEADZONE_DEG && std::abs(pitchDeg) < DEADZONE_DEG) return;

	// panView takes (deltaAzimuth, deltaAltitude) in degrees. Turning the head right
	// (negative yaw delta by the convention above) must move the view's azimuth by the
	// same amount in the opposite direction.
	mm->panView(-yawDeg, -pitchDeg);

	// Keep the tracker's prediction honest by feeding back how stale the newest sample is:
	// that age IS the motion-to-photon latency we are trying to hide.
	headTracking->setMeasuredLatency(headTracking->getSampleAge());
}

void StelCardboardRenderer::snapTurn(bool toLeft)
{
	// Discrete rotation of the view. Snap turning measured roughly 40-50% less sickness
	// than smooth turning, because the vestibular system never registers the sustained
	// rotation that a smooth pan would present. The jump is short enough to read as a cut
	// rather than as motion.
	StelCore* core = StelApp::getInstance().getCore();
	if (!core) return;

	StelMovementMgr* mm = core->getMovementMgr();
	if (!mm) return;

	const double step = qDegreesToRadians(snapTurnAngle) * (toLeft ? 1.0 : -1.0);

	Vec3d dir = mm->getViewDirectionJ2000();
	// VecMath deletes length()/lengthSquared() to force norm()/normSquared().
	if (dir.normSquared() < 1e-12) return;

	// Rotate about the observer's up axis, which is what a user expects from a yaw input.
	const Vec3d up(0.0, 0.0, 1.0);
	dir.transfo4d(Mat4d::rotation(up, step));
	mm->setViewDirectionJ2000(dir);
}
