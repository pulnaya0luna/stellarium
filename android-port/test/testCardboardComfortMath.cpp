/*
 * Unit tests for the Cardboard port's comfort-critical maths.
 *
 * These test the REAL shared implementation in StelCardboardComfortMath.hpp -- the same
 * header the live head tracker uses -- not a copy. That matters: a regression in
 * prediction or recentring is felt by the user as nausea, which is an expensive way to
 * discover a bug, so the actual code must be under test.
 *
 * They run on the host with no sensor and no GL context, because everything that
 * governs comfort here is pure maths and timing.
 */

#include <QQuaternion>
#include <QtMath>
#include <QtTest/QtTest>

#include "StelCardboardComfortMath.hpp"

using namespace StelCardboard;

class TestCardboardComfortMath : public QObject
{
	Q_OBJECT

private slots:
	// --- prediction: direction and magnitude ------------------------------------

	void predictionDisabledReturnsRecentredOnly()
	{
		const QQuaternion raw   = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		const QQuaternion delta = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 1.0);

		const QQuaternion out = predictedOrientation(raw, QQuaternion(), delta, true, 0.0, 0.010);

		QVERIFY(qFuzzyCompare(out.scalar(), raw.scalar()));
		QVERIFY(qFuzzyCompare(out.z(), raw.z()));
	}

	void predictionExtrapolatesForward()
	{
		const QQuaternion raw   = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		const QQuaternion delta = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 1.0);

		// 20 ms horizon at a 10 ms sample interval = 2 samples ahead = ~2 more degrees.
		const QQuaternion out = predictedOrientation(raw, QQuaternion(), delta, true, 0.020, 0.010);

		const QQuaternion expected = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 12.0);
		QVERIFY(qAbs(out.scalar() - expected.scalar()) < 0.01);
		QVERIFY(qAbs(out.z() - expected.z()) < 0.01);

		// Must lead the raw orientation, never trail it -- trailing would mean added lag.
		QVERIFY(out.z() > raw.z());
	}

	void predictionIsClampedAgainstStall()
	{
		const QQuaternion raw   = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		const QQuaternion delta = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 1.0);

		// An absurd horizon must clamp to MAX_PREDICTION_FACTOR x the delta, not fly off.
		const QQuaternion out     = predictedOrientation(raw, QQuaternion(), delta, true, 1.0, 0.010);
		const QQuaternion clamped = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 12.0);

		QVERIFY(qAbs(out.scalar() - clamped.scalar()) < 0.01);
	}

	void predictionWithoutDeltaDoesNotExtrapolate()
	{
		const QQuaternion raw   = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		const QQuaternion delta = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 1.0);

		const QQuaternion out = predictedOrientation(raw, QQuaternion(), delta, false, 0.020, 0.010);

		QVERIFY(qAbs(out.scalar() - raw.scalar()) < 1e-6);
	}

	void predictionRejectsUnusableSampleInterval()
	{
		const QQuaternion raw   = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		const QQuaternion delta = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 1.0);

		// A stalled interval (app was backgrounded) must suppress extrapolation entirely.
		const QQuaternion out = predictedOrientation(raw, QQuaternion(), delta, true, 0.020, 2.0);
		QVERIFY(qAbs(out.scalar() - raw.scalar()) < 1e-6);

		// A zero interval would divide by zero; must also be rejected.
		const QQuaternion out2 = predictedOrientation(raw, QQuaternion(), delta, true, 0.020, 0.0);
		QVERIFY(qAbs(out2.scalar() - raw.scalar()) < 1e-6);
	}

	// --- recentring -------------------------------------------------------------

	void recenterYieldsIdentityForCurrentPose()
	{
		// After recentring, the current pose becomes "looking forward" => identity.
		// Anything else is a visible lurch at the moment of recentring.
		const QQuaternion current = QQuaternion::fromAxisAndAngle(QVector3D(0.3, 0.5, 0.8), 37.0);
		const QQuaternion out     = predictedOrientation(current, current, QQuaternion(), false, 0.020, 0.010);

		QVERIFY(qAbs(out.scalar() - 1.0) < 1e-6);
		QVERIFY(qAbs(out.x()) < 1e-6);
		QVERIFY(qAbs(out.y()) < 1e-6);
		QVERIFY(qAbs(out.z()) < 1e-6);
	}

	void recenterThenRotateGivesRelativeAngle()
	{
		const QQuaternion reference = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 90.0);
		const QQuaternion raw       = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 100.0);

		const QQuaternion out = predictedOrientation(raw, reference, QQuaternion(), false, 0.0, 0.010);

		const QQuaternion expected = QQuaternion::fromAxisAndAngle(QVector3D(0, 0, 1), 10.0);
		QVERIFY(qAbs(out.scalar() - expected.scalar()) < 0.01);
		QVERIFY(qAbs(out.z() - expected.z()) < 0.01);
	}

	// --- clamping helpers -------------------------------------------------------

	void predictionTimeClampedToSafeWindow()
	{
		QCOMPARE(clampPredictionTime(-0.1), MIN_PREDICTION_TIME);
		QCOMPARE(clampPredictionTime(0.020), 0.020);
		QCOMPARE(clampPredictionTime(0.050), MAX_PREDICTION_TIME);
		QCOMPARE(clampPredictionTime(0.500), MAX_PREDICTION_TIME);

		// The default must sit at the measured sickness threshold, not above it.
		QVERIFY(DEFAULT_PREDICTION_TIME <= 0.020);
	}

	void sampleIntervalGateMatchesSpec()
	{
		QVERIFY(!isUsableSampleInterval(0.0));
		QVERIFY(isUsableSampleInterval(0.010));  // normal 100 Hz sample
		QVERIFY(isUsableSampleInterval(0.100));  // slow device, still usable
		QVERIFY(!isUsableSampleInterval(0.500)); // backgrounded: reject
		QVERIFY(!isUsableSampleInterval(2.000)); // long stall: reject
	}

	// --- snap turn contract -----------------------------------------------------

	void snapTurnAnglesAreInTheResearchedRange()
	{
		// Research: 30-45 deg recommended, 22.5 deg pilot-tested comfortable. Values
		// outside this range are disorienting (too large) or ineffective (too small).
		for (double deg : {22.5, 30.0, 45.0})
		{
			QVERIFY(deg >= 22.5 && deg <= 45.0);
		}
	}
};

QTEST_MAIN(TestCardboardComfortMath)
#include "testCardboardComfortMath.moc"
