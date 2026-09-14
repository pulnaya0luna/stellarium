/*
 * Stellarium Cardboard port — comfort-critical orientation maths.
 *
 * Deliberately a small, dependency-light, header-only unit so that BOTH the live
 * head tracker and the host unit tests exercise the *same* code. The comfort
 * behaviour of this port (prediction, clamping, recentring) is the part where a
 * regression is felt by the user as nausea, so it must be tested for real rather
 * than through a mirrored copy in the test file.
 *
 * See notes/VR-COMFORT-RESEARCH.md for the evidence behind these rules.
 */

#ifndef STELCARDBOARDCOMFORTMATH_HPP
#define STELCARDBOARDCOMFORTMATH_HPP

#include <QQuaternion>
#include <QtMath>

namespace StelCardboard
{

//! Safe window for the prediction horizon, in seconds.
//! Below 0 would render stale orientation (visible lag); far above 50 ms would
//! overshoot real motion. 20 ms is the default because a US Army study found
//! ~19 ms to be below the threshold for eliciting sickness.
constexpr double MIN_PREDICTION_TIME     = 0.0;
constexpr double MAX_PREDICTION_TIME     = 0.050;
constexpr double DEFAULT_PREDICTION_TIME = 0.020;

//! Sample intervals outside this range are rejected rather than integrated.
//! A long gap (app backgrounded, a stall) must not produce a large delta, or the
//! view would fling on resume -- exactly the unexpected motion that causes sickness.
constexpr double MIN_SAMPLE_INTERVAL = 1e-5;
constexpr double MAX_SAMPLE_INTERVAL = 0.25;

//! Prediction is never allowed to extrapolate more than this multiple of the last
//! observed angular delta. Without a clamp, a slow sensor would be extrapolated into
//! invented motion.
constexpr double MAX_PREDICTION_FACTOR = 2.0;

//! Clamp a requested prediction horizon into the safe window.
inline double clampPredictionTime(double seconds)
{
	if (seconds < MIN_PREDICTION_TIME) return MIN_PREDICTION_TIME;
	if (seconds > MAX_PREDICTION_TIME) return MAX_PREDICTION_TIME;
	return seconds;
}

//! Is this sample interval usable for delta/prediction maths?
inline bool isUsableSampleInterval(double dt)
{
	return dt > MIN_SAMPLE_INTERVAL && dt < MAX_SAMPLE_INTERVAL;
}

//! Apply the recentring reference and the forward prediction to a raw orientation.
//!
//! @param raw            newest fused orientation (world -> device)
//! @param reference      orientation captured at the last recenter()
//! @param lastDelta      angular delta since the previous sample
//! @param haveDelta      whether lastDelta is meaningful
//! @param predictionTime horizon in seconds (already clamped by the caller, or not)
//! @param sampleInterval measured interval between the last two samples
//! @return orientation to render with
inline QQuaternion predictedOrientation(const QQuaternion& raw, const QQuaternion& reference,
                                        const QQuaternion& lastDelta, bool haveDelta, double predictionTime,
                                        double sampleInterval)
{
	// Re-express in the recentred frame first. After recenter(), the pose that was
	// current must come out as identity -- any offset here is a visible lurch.
	QQuaternion q = raw * reference.conjugated();

	if (!haveDelta || predictionTime <= 0.0 || !isUsableSampleInterval(sampleInterval)) return q;

	double frac = predictionTime / sampleInterval;
	if (frac < 0.0) frac = 0.0;
	if (frac > MAX_PREDICTION_FACTOR) frac = MAX_PREDICTION_FACTOR;

	// Slerp from identity toward the observed delta, applied in the device frame.
	const QQuaternion step = QQuaternion::slerp(QQuaternion(), lastDelta, float(frac));
	return q * step;
}

} // namespace StelCardboard

#endif // STELCARDBOARDCOMFORTMATH_HPP
