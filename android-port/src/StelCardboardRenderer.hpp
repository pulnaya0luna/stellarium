/*
 * Stellarium Cardboard port — VR presentation module.
 *
 * Responsibilities:
 *   1. Own the head tracker and keep it running.
 *   2. Install/remove the Cardboard stereo viewport effect at the right moment.
 *   3. Provide the comfort controls (snap turn, vignette policy, IPD).
 *
 * ARCHITECTURE
 * ------------
 * This is a StelModule so it uses upstream's own extension points and the patch stays
 * small and rebase-able. It does NOT modify StelProjector, StelPainter or StelCore.
 *
 * The actual stereo compositing lives in StelCardboardViewportEffect, which plugs into
 * upstream's post-process hook (StelViewportEffect::paintViewportBuffer). That hook is
 * the single place where the rendered scene buffer reaches the screen, so compositing
 * there leaves the whole engine untouched.
 *
 * COMFORT DECISIONS (evidence in notes/VR-COMFORT-RESEARCH.md)
 * -----------------------------------------------------------
 *   - FOV is FIXED at a Cardboard-appropriate value. It never breathes with frame rate or
 *     movement; a pumping FOV is a known discomfort source and would also break the lens
 *     distortion mapping.
 *   - The vignette is applied ONLY during app-initiated view motion (scripted pans,
 *     goto-object animation, snap-turn transitions). It is explicitly NOT applied during
 *     head rotation: the research notes that vignetting a head-driven view change can make
 *     sickness worse, because the vignette conflicts with the rotation itself. This is the
 *     opposite of the naive reading of consumer VR comfort settings, and it matters here
 *     because Stellarium is a head-rotation-dominated app.
 *   - Snap turn (default 30 degrees) for controller-driven yaw, never a smooth pan.
 *   - The landscape/horizon stays enabled by default: it is a rest frame, a stable visual
 *     anchor that measurably delays symptom onset.
 */

#ifndef STELCARDBOARDRENDERER_HPP
#define STELCARDBOARDRENDERER_HPP

#include "StelModule.hpp"
#include <QQuaternion>
#include <QSize>
#include <QString>

class StelCardboardHeadTracking;
class StelCardboardViewportEffect;

//! Cardboard VR presentation module: head tracking + stereo compositing host.
class StelCardboardRenderer : public StelModule
{
	Q_OBJECT

public:
	StelCardboardRenderer();
	~StelCardboardRenderer() override;

	// StelModule interface
	void init() override;
	void draw(StelCore* core) override;
	double getCallOrder(StelModuleActionName actionName) const override;

	//! Enable/disable stereo output. Disabled removes the viewport effect entirely, so
	//! mono rendering is byte-for-byte upstream behaviour.
	bool isStereoEnabled() const { return stereoEnabled; }
	void setStereoEnabled(bool b);

	//! Physical lens/screen description. These must match the actual viewer: they drive
	//! both the eye offsets and the distortion correction, and a mismatch shows up as eye
	//! strain and headache independently of nausea.
	void setInterPupillaryDistance(double mm);
	double getInterPupillaryDistance() const { return ipdMm; }

	//! Comfort vignette strength for APP-INITIATED motion only. 0..1. Not used during
	//! head rotation -- see the header comment.
	void setVignetteStrength(float s);
	float getVignetteStrength() const { return vignetteStrength; }

	//! Snap-turn angle in degrees for controller-driven yaw (0 disables snap turn).
	void setSnapTurnAngle(double deg) { snapTurnAngle = deg; }
	double getSnapTurnAngle() const { return snapTurnAngle; }

	//! Rotate the view by one snap step. Uses discrete rotation, never a smooth pan:
	//! continuous virtual rotation is the strongest sickness driver there is.
	void snapTurn(bool toLeft);

	//! Access the tracker (diagnostics overlay, tests).
	StelCardboardHeadTracking* getHeadTracking() const { return headTracking; }

	//! Human-readable status line for diagnostics.
	QString getStatusText() const;

private:
	void installEffect();
	void removeEffect();
	//! Rotate the view by the change in head orientation since the previous frame.
	void applyHeadOrientation(StelCore* core);

	StelCardboardHeadTracking* headTracking = nullptr;

	bool stereoEnabled   = true;
	bool effectInstalled = false;

	double ipdMm           = 64.0; //!< average adult IPD; the classic Cardboard value
	float vignetteStrength = 0.0f; //!< 0 by default: head rotation gets no vignette
	double snapTurnAngle   = 30.0; //!< degrees; research suggests 30-45
	double vrFov           = 95.0; //!< degrees; Cardboard lenses are roughly 90-100

	QSize screenSize;

	//! Previous frame's predicted orientation, for computing the increment. The first
	//! sample is adopted as a baseline without moving the view, so the sky does not lurch
	//! on the first frame.
	QQuaternion lastOrientation;
	bool haveLastOrientation = false;
};

#endif // STELCARDBOARDRENDERER_HPP
