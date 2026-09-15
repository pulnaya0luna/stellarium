/*
 * Stellarium Cardboard port — lens distortion maths.
 *
 * Header-only and dependency-light so the unit tests exercise the REAL code rather than
 * a copy, the same way StelCardboardComfortMath.hpp is structured.
 *
 * WHY THIS IS TESTED
 * ------------------
 * The distortion mapping is a comfort-critical correctness property, not a cosmetic one.
 * If the pre-distortion is wrong the two eyes are shown subtly different geometry and the
 * user's visual system has to fight it, which produces eye strain and headache on its own
 * before any nausea enters the picture. It is also easy to get silently wrong: the error
 * mode is a plausible-looking image that is the wrong shape, invisible in a screenshot.
 *
 * THE MODEL
 * ---------
 * Cardboard lenses are modelled with the Brown-Conrady radial polynomial:
 *
 *     r_d = r_u * (1 + k1*r_u^2 + k2*r_u^4)
 *
 * A Cardboard lens is a barrel lens, so k1 < 0: the lens pulls the periphery inward.
 *
 * HOW IT IS APPLIED — AND WHY NOT BY INVERSION
 * --------------------------------------------
 * The image must be pre-warped so the lens cancels it. The natural-looking way to do that
 * is to invert the polynomial and ask, for each output pixel, "which source pixel ends up
 * here?". That approach is WRONG and was caught by the unit tests:
 *
 *   With k1 = -0.22 the forward polynomial r(1 + k1*r^2) is NOT monotonic — it peaks
 *   around r = 1.23 at a value of about 0.82 and then decreases. So for any target radius
 *   above ~0.82 there is no solution at all, and a numerical inversion (Newton) returns
 *   garbage. That is most of the screen, not just the corners.
 *
 * The correct mapping, and the one the Cardboard SDK uses, applies the polynomial
 * FORWARD to the output coordinate:
 *
 *     r_sample = r_out * (1 + k1*r_out^2 + k2*r_out^4)
 *
 * i.e. the output pixel samples the source texture at a slightly smaller radius, which
 * magnifies the periphery and pre-stretches the image the way the lens will compress it.
 * This needs no inversion, is exact, and is monotonic everywhere the screen actually is:
 * the scale factor only reaches zero at r = sqrt(-1/k1) ~ 2.13 for k1 = -0.22, well
 * beyond the screen corner at sqrt(2) ~ 1.41.
 */

#ifndef STELCARDBOARDDISTORTION_HPP
#define STELCARDBOARDDISTORTION_HPP

#include <cmath>

namespace StelCardboard
{

//! Largest normalised radius present on screen: the corner of a -1..1 square.
inline constexpr float MAX_SCREEN_RADIUS = 1.41421356f; // sqrt(2)

//! Apply the Brown-Conrady radial polynomial to a normalised radius.
//! r_d = r_u * (1 + k1*r_u^2 + k2*r_u^4)
inline float distortRadius(float rU, float k1, float k2)
{
	const float r2 = rU * rU;
	return rU * (1.0f + k1 * r2 + k2 * r2 * r2);
}

//! Radius beyond which the 2-term model stops being valid, so the mapping must clamp.
//!
//! For a barrel lens (k1 < 0) the polynomial peaks where its derivative vanishes,
//! 1 + 3*k1*r^2 = 0, i.e. at r = sqrt(-1/(3*k1)) -- about 1.23 for k1 = -0.22. Past that
//! point the model would fold the image back on itself, which shows as corruption rather
//! than as a plausible picture. Real lens models add higher-order terms instead; the
//! honest fix for a 2-term model is to clamp and stop trusting it.
//!
//! A Cardboard lens only ever sees out to roughly the inscribed circle (r = 1 for a
//! ~90-100 degree lens), comfortably inside this limit, so the clamp never affects
//! visible content -- it only prevents nonsense in the hidden corners.
inline float monotonicLimit(float k1)
{
	if (k1 >= 0.0f) return MAX_SCREEN_RADIUS; // pincushion does not fold in this range
	const float limit = std::sqrt(-1.0f / (3.0f * k1));
	return limit < MAX_SCREEN_RADIUS ? limit : MAX_SCREEN_RADIUS;
}

//! Radial scale factor applied to a point at normalised radius r.
//! This is the factor by which an output pixel's radius is scaled to find the source
//! texel it must sample.
inline float radialScale(float r, float k1, float k2)
{
	const float r2 = r * r;
	return 1.0f + k1 * r2 + k2 * r2 * r2;
}

//! Map an output coordinate (normalised device coords, -1..1 across one eye) to the
//! source coordinate that must be sampled to pre-compensate the lens distortion.
//!
//! Applied forward, not by inversion — see the header comment for why inverting the
//! polynomial is wrong. The radius is clamped to the model's monotonic limit so the
//! mapping is never allowed to fold.
inline void preDistortNormalised(float& x, float& y, float k1, float k2)
{
	const float r = std::sqrt(x * x + y * y);
	if (r < 1e-7f) return; // at the optical centre the distortion is zero by definition

	// Clamp before scaling: beyond the monotonic limit the polynomial would reverse
	// direction and fold the image.
	const float limit    = monotonicLimit(k1);
	const float rClamped = (r > limit) ? limit : r;

	float scale = radialScale(rClamped, k1, k2);
	// A non-positive scale would mirror the image through the centre. Unreachable for any
	// sane coefficient set, but a user-supplied extreme value must not flip the sky.
	if (scale < 0.0f) scale = 0.0f;

	// Clamping the radius means the outermost sliver all samples from the limit radius.
	// That is a deliberate degradation in a region the lens does not show, and it is far
	// better than folding.
	const float outR = rClamped * scale;
	const float k    = (r > 1e-7f) ? (outR / r) : 0.0f;
	x *= k;
	y *= k;
}

//! Eye viewport for one half of a side-by-side stereo frame.
//! @param eye 0 = left, 1 = right
//! @param fullWidth full display width in pixels
//! @param height display height in pixels
struct EyeViewport
{
	int x;
	int y;
	int width;
	int height;
};

//! Compute one eye's viewport within a side-by-side stereo frame.
//!
//! The two eyes must tile the display EXACTLY: a gap shows as a black seam down the
//! middle, and an overlap means the eyes disagree about geometry at the centre, which is
//! a convergence error the user feels as eye strain. So the right eye takes the
//! remainder rather than a second truncated half — with an odd width, integer division
//! would otherwise drop a column.
inline EyeViewport eyeViewport(int eye, int fullWidth, int height)
{
	const int halfWidth = fullWidth / 2;
	if (eye == 0) return EyeViewport{0, 0, halfWidth, height};
	// Right eye gets everything from the midpoint to the edge, remainder included.
	return EyeViewport{halfWidth, 0, fullWidth - halfWidth, height};
}

//! Horizontal offset, in pixels, that each eye's view centre sits from its half's centre.
//!
//! IPD is applied as a lateral shift of the observer, but a small residual screen-space
//! offset is needed so the eye's optical axis lines up with the lens centre in a Cardboard
//! viewer. Getting this wrong is a convergence error: the eyes must rotate slightly to
//! fuse the image, which is fatiguing over a session.
inline int ipdScreenOffsetPixels(double ipdMm, double screenWidthMm, int eyeHalfWidthPx)
{
	if (screenWidthMm <= 0.0) return 0;
	// Half the IPD, converted from physical millimetres to pixels of this half-viewport.
	const double halfIpdMm = ipdMm / 2.0;
	const double pxPerMm   = double(eyeHalfWidthPx) / screenWidthMm;
	return int(std::lround(halfIpdMm * pxPerMm));
}

} // namespace StelCardboard

#endif // STELCARDBOARDDISTORTION_HPP
