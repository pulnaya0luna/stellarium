/*
 * Unit tests for the Cardboard lens distortion maths.
 *
 * These test the REAL implementation in StelCardboardDistortion.hpp — the same header the
 * renderer uses — not a copy.
 *
 * Why this is worth testing rather than eyeballing: the failure mode of a wrong
 * pre-distortion is a plausible-looking image that is the wrong shape. You cannot see it
 * in a screenshot. What you get instead is eye strain and headache, and the user has no
 * way to tell you the geometry is off.
 *
 * These tests caught a real design error: the first implementation inverted the
 * Brown-Conrady polynomial numerically, which fails for most of the screen because the
 * polynomial is not monotonic past r ~ 1.23. The monotonicity and positivity tests below
 * are what surfaced it.
 */

#include <QtMath>
#include <QtTest/QtTest>

#include "StelCardboardDistortion.hpp"

using namespace StelCardboard;

class TestCardboardDistortion : public QObject
{
	Q_OBJECT

private slots:
	// --- forward distortion -----------------------------------------------------

	void forwardDistortionIsIdentityAtCentre()
	{
		// At the optical centre the radius is zero, so any coefficient set must leave it
		// unchanged. This is the one point where the mapping must be exactly identity.
		QCOMPARE(distortRadius(0.0f, -0.22f, 0.0f), 0.0f);
		QCOMPARE(distortRadius(0.0f, -0.5f, 0.1f), 0.0f);
	}

	void barrelLensPullsRadiusInward()
	{
		// A Cardboard lens is a barrel lens (k1 < 0), which compresses the periphery.
		// A larger radius must therefore map to a SMALLER distorted radius.
		const float rU = 0.5f;
		const float rD = distortRadius(rU, -0.22f, 0.0f);
		QVERIFY2(rD < rU, "negative k1 must shrink the radius (barrel distortion)");
	}

	void pincushionLensPushesRadiusOutward()
	{
		// Sanity check the sign convention: positive k1 must do the opposite.
		const float rU = 0.5f;
		const float rD = distortRadius(rU, 0.22f, 0.0f);
		QVERIFY2(rD > rU, "positive k1 must expand the radius (pincushion distortion)");
	}

	void scaleIsIdentityAtCentre() { QCOMPARE(radialScale(0.0f, -0.22f, 0.0f), 1.0f); }

	// --- the properties that make the mapping usable over the whole screen -------

	void scaleStaysPositiveAcrossTheWholeScreen()
	{
		// THE critical property. A non-positive scale factor would mirror the sample
		// through the centre, folding the image back on itself -- visible corruption
		// across the display, not just at the edges.
		//
		// This is checked out to the screen corner, which is the largest radius that can
		// actually be rendered.
		for (float k1 : {-0.22f, -0.30f, -0.10f, 0.10f})
		{
			for (int i = 0; i <= 40; ++i)
			{
				const float r = MAX_SCREEN_RADIUS * float(i) / 40.0f;
				const float s = radialScale(r, k1, 0.0f);
				QVERIFY2(s > 0.0f,
				         qPrintable(QStringLiteral("scale went non-positive at r=%1, k1=%2: %3")
				                            .arg(r)
				                            .arg(k1)
				                            .arg(s)));
			}
		}
	}

	void mappingIsMonotonicAcrossTheWholeScreen()
	{
		// Larger output radius must always give a larger sample radius, otherwise the
		// image folds. The earlier inverting implementation violated this past r ~ 1.23;
		// the forward mapping must hold everywhere on screen.
		//
		// This tests the actual mapping (preDistortNormalised), which clamps past the
		// model's monotonic limit. The raw polynomial does peak at r ~ 1.23 for k1 = -0.22
		// -- that is a property of the 2-term model, and the clamp is what makes the
		// mapping usable over the full screen.
		const float k1 = -0.22f;
		float prev     = -1.0f;
		for (int i = 0; i <= 60; ++i)
		{
			const float r = MAX_SCREEN_RADIUS * float(i) / 60.0f;
			float x       = r;
			float y       = 0.0f;
			preDistortNormalised(x, y, k1, 0.0f);
			const float out = std::sqrt(x * x + y * y);
			QVERIFY2(out >= prev - 1e-6f,
			         qPrintable(QStringLiteral("mapping not monotonic at r=%1 (%2 < %3)")
			                            .arg(r)
			                            .arg(out)
			                            .arg(prev)));
			prev = out;
		}
	}

	void mappingStaysMonotonicForSeveralCoefficients()
	{
		// The same property must hold for any coefficient a user might set, not just the
		// default.
		for (float k1 : {-0.10f, -0.22f, -0.30f, -0.50f, -0.90f})
		{
			float prev = -1.0f;
			for (int i = 0; i <= 40; ++i)
			{
				const float r = MAX_SCREEN_RADIUS * float(i) / 40.0f;
				float x       = r;
				float y       = 0.0f;
				preDistortNormalised(x, y, k1, 0.0f);
				const float out = std::sqrt(x * x + y * y);
				QVERIFY2(out >= prev - 1e-6f,
				         qPrintable(QStringLiteral("not monotonic at k1=%1 r=%2").arg(k1).arg(r)));
				prev = out;
			}
		}
	}

	// --- 2D mapping -------------------------------------------------------------

	void preDistortIsIdentityAtCentre()
	{
		float x = 0.0f;
		float y = 0.0f;
		preDistortNormalised(x, y, -0.22f, 0.0f);
		QCOMPARE(x, 0.0f);
		QCOMPARE(y, 0.0f);
	}

	void preDistortPreservesDirection()
	{
		// The warp is purely radial: it must scale the radius but never rotate the point.
		// A rotational error would show as a twist between the eyes.
		float x               = 0.6f;
		float y               = 0.8f;
		const float origAngle = std::atan2(y, x);
		preDistortNormalised(x, y, -0.22f, 0.0f);
		const float newAngle = std::atan2(y, x);
		QVERIFY2(qAbs(newAngle - origAngle) < 1e-5f, "pre-distortion must not rotate points");
	}

	void barrelPreDistortPullsSamplesInward()
	{
		// For a barrel lens (k1<0) the pre-warp must sample at a SMALLER radius, which
		// magnifies the periphery so the lens' inward compression cancels it.
		float x            = 0.8f;
		float y            = 0.0f;
		const float before = std::sqrt(x * x + y * y);
		preDistortNormalised(x, y, -0.22f, 0.0f);
		const float after = std::sqrt(x * x + y * y);
		QVERIFY2(after < before, "barrel pre-warp must sample inside the output radius");
	}

	void pincushionPreDistortPushesSamplesOutward()
	{
		float x            = 0.5f;
		float y            = 0.0f;
		const float before = std::sqrt(x * x + y * y);
		preDistortNormalised(x, y, 0.22f, 0.0f);
		const float after = std::sqrt(x * x + y * y);
		QVERIFY2(after > before, "positive k1 must sample outside the output radius");
	}

	void preDistortStaysFiniteForExtremeCoefficients()
	{
		// A user could set an extreme coefficient; the maths must stay finite rather than
		// emitting NaN into vertex positions (which would make the mesh disappear).
		for (float k1 : {-0.9f, -0.5f, 0.5f, 1.0f})
		{
			float x = 1.2f;
			float y = 0.7f;
			preDistortNormalised(x, y, k1, 0.0f);
			QVERIFY2(std::isfinite(x) && std::isfinite(y), "pre-distortion must stay finite");
		}
	}

	void preDistortNeverMirrorsWithExtremeCoefficients()
	{
		// Even with an absurd k1 that drives the scale negative, the result must stay in
		// front of the optical centre rather than flipping through it.
		float x = 1.0f;
		float y = 0.0f;
		preDistortNormalised(x, y, -5.0f, 0.0f);
		QVERIFY2(x >= 0.0f, "a non-positive scale must clamp, not mirror");
	}

	// --- eye viewports ----------------------------------------------------------

	void eyeViewportsTileTheScreenExactly()
	{
		// The two eyes must partition the display with no gap and no overlap. A gap shows
		// as a black seam between the eyes; an overlap means the eyes disagree about
		// geometry at the centre, which is a convergence error.
		const int w             = 1080;
		const int h             = 2400;
		const EyeViewport left  = eyeViewport(0, w, h);
		const EyeViewport right = eyeViewport(1, w, h);

		QCOMPARE(left.x, 0);
		QCOMPARE(left.width, w / 2);
		QCOMPARE(right.x, w / 2);
		QCOMPARE(left.x + left.width, right.x);
		QCOMPARE(right.x + right.width, w);
		QCOMPARE(left.height, h);
		QCOMPARE(right.height, h);
	}

	void eyeViewportsHandleOddWidth()
	{
		// An odd width must still tile exactly. Integer division would drop the last
		// column, leaving an unpainted 1px strip at the right edge of the display.
		const int w             = 1079;
		const EyeViewport left  = eyeViewport(0, w, 100);
		const EyeViewport right = eyeViewport(1, w, 100);
		QCOMPARE(left.x + left.width, right.x);
		QCOMPARE(right.x + right.width, w);
		QVERIFY(right.width >= left.width);
	}

	void eyeViewportsTileExactlyForManyWidths()
	{
		// The tiling property must hold for any width, not just the two probed above.
		for (int w = 100; w <= 120; ++w)
		{
			const EyeViewport left  = eyeViewport(0, w, 50);
			const EyeViewport right = eyeViewport(1, w, 50);
			QCOMPARE(left.x + left.width, right.x);
			QCOMPARE(right.x + right.width, w);
			QVERIFY2(left.width > 0 && right.width > 0, "both eyes must get pixels");
		}
	}

	// --- IPD --------------------------------------------------------------------

	void ipdOffsetIsZeroWithoutScreenSize()
	{
		// Guard against a divide-by-zero when the physical size is unknown, which is the
		// case on many phones.
		QCOMPARE(ipdScreenOffsetPixels(64.0, 0.0, 540), 0);
	}

	void ipdOffsetScalesWithScreenSize()
	{
		// Half of a 64 mm IPD on a 120 mm wide screen is 26.7% of the width. Expressed in
		// pixels of a 540 px half-viewport that is ~144 px. Getting this wrong is a
		// convergence error the user feels as eye strain.
		const int px = ipdScreenOffsetPixels(64.0, 120.0, 540);
		QVERIFY2(px > 130 && px < 160, qPrintable(QStringLiteral("expected ~144 px, got %1").arg(px)));
	}

	void largerIpdGivesLargerOffset()
	{
		const int narrow = ipdScreenOffsetPixels(58.0, 120.0, 540);
		const int wide   = ipdScreenOffsetPixels(72.0, 120.0, 540);
		QVERIFY2(wide > narrow, "a larger IPD must produce a larger screen offset");
	}

	// --- eye-local mapping (must agree with what the stereo renderer draws) ---------
	//
	// These pin the transform that distortXY() and any picking/UI code depend on. The
	// original distortXY() used full-screen normalised coordinates while the renderer used
	// eye-local ones, so the same pixel meant two different things and overlaid UI or
	// picking would have been off by roughly half an eye width. These tests make that
	// mismatch impossible to reintroduce silently.

	void eyeCentreMapsToEyeLocalOrigin()
	{
		// The centre of EACH eye must map to x = 0 in eye-local space. This is exactly the
		// property the old implementation violated: it produced -0.5 for the left eye and
		// +0.5 for the right.
		const int w = 2400;
		const int h = 1000;
		float nx    = 0.0f;
		float ny    = 0.0f;

		screenToEyeLocal(w * 0.25, h * 0.5, w, h, nx, ny);
		QVERIFY2(std::abs(nx) < 1e-4f, qPrintable(QStringLiteral("left eye centre nx=%1").arg(nx)));
		QVERIFY2(std::abs(ny) < 1e-4f, qPrintable(QStringLiteral("left eye centre ny=%1").arg(ny)));

		screenToEyeLocal(w * 0.75, h * 0.5, w, h, nx, ny);
		QVERIFY2(std::abs(nx) < 1e-4f, qPrintable(QStringLiteral("right eye centre nx=%1").arg(nx)));
	}

	void eyeLocalSpansTheAspectOnXAndUnitOnY()
	{
		// Each eye's own half must map to x in -aspect..+aspect and y in -1..+1, matching
		// setupBuffers(). Off-by-a-factor here is the silent "plausible but wrong shape"
		// class of bug.
		//
		// Note on boundaries: x == w/2 is the FIRST pixel of the RIGHT eye, not the right
		// edge of the left one, so it maps to -aspect. The left eye's last pixel is w/2 - 1,
		// which is one pixel short of +aspect. Test the actual last pixel, not the boundary.
		const int w        = 2400;
		const int h        = 1000;
		const float aspect = float(w / 2) / float(h);

		float nx = 0.0f;
		float ny = 0.0f;

		screenToEyeLocal(0.0, h * 0.5, w, h, nx, ny); // far left edge of the left eye
		QVERIFY2(std::abs(nx + aspect) < 1e-4f,
		         qPrintable(QStringLiteral("left edge nx=%1 want %2").arg(nx).arg(-aspect)));

		screenToEyeLocal(double(w / 2 - 1), h * 0.5, w, h, nx, ny); // left eye's LAST pixel
		QVERIFY2(nx > aspect - 0.01f && nx < aspect,
		         qPrintable(QStringLiteral("left eye last pixel nx=%1").arg(nx)));

		screenToEyeLocal(double(w) * 0.5, h * 0.5, w, h, nx, ny); // first pixel of the right eye
		QVERIFY2(std::abs(nx + aspect) < 1e-4f,
		         qPrintable(QStringLiteral("right eye first pixel nx=%1").arg(nx)));

		screenToEyeLocal(double(w) * 0.5, 0.0, w, h, nx, ny); // top edge
		QVERIFY2(std::abs(ny - 1.0f) < 1e-4f, qPrintable(QStringLiteral("top ny=%1").arg(ny)));

		screenToEyeLocal(double(w) * 0.5, double(h), w, h, nx, ny); // bottom edge
		QVERIFY2(std::abs(ny + 1.0f) < 1e-4f, qPrintable(QStringLiteral("bottom ny=%1").arg(ny)));
	}

	void eyeLocalRoundTripsThroughScreenSpace()
	{
		// screenToEyeLocal and eyeLocalToScreen must be inverses, or a picking hit test
		// would drift from the drawn geometry.
		//
		// Tolerance: the pipeline stores nx/ny as float (they travel through the vertex
		// buffer and shader as float), so a round trip loses float precision. At 2400 px
		// that measured ~3e-6 px, so 0.01 px is a realistic bound and still far tighter
		// than anything picking needs.
		const int w   = 2400;
		const int h   = 1000;
		const int eye = 1;

		float nx            = 0.0f;
		float ny            = 0.0f;
		const double startX = w * 0.8;
		const double startY = h * 0.3;
		screenToEyeLocal(startX, startY, w, h, nx, ny);

		double backX = 0.0;
		double backY = 0.0;
		eyeLocalToScreen(eye, nx, ny, w, h, backX, backY);

		QVERIFY2(std::abs(backX - startX) < 0.01,
		         qPrintable(QStringLiteral("x round-trip %1 vs %2").arg(backX).arg(startX)));
		QVERIFY2(std::abs(backY - startY) < 0.01,
		         qPrintable(QStringLiteral("y round-trip %1 vs %2").arg(backY).arg(startY)));
	}

	void distortionKeepsTheEyeCentreFixed()
	{
		// The radial warp is zero at the optical centre, so a point at the eye's centre must
		// not move at all. If it did, the whole view would be shifted.
		float sx = 0.0f;
		float sy = 0.0f;
		preDistortNormalised(sx, sy, -0.22f, 0.0f);
		QCOMPARE(sx, 0.0f);
		QCOMPARE(sy, 0.0f);
	}
};

QTEST_MAIN(TestCardboardDistortion)
#include "testCardboardDistortion.moc"
