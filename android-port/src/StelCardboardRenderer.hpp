/*
 * Stellarium Cardboard port — stereoscopic Cardboard renderer.
 *
 * Turns one Stellarium frame into a side-by-side stereo pair for a Cardboard viewer.
 *
 * Architecture note: this is deliberately a StelModule registered with the normal
 * module manager, so it uses upstream's own extension points and the patch stays
 * small and rebase-able. It does not modify StelProjector or StelCore.
 *
 * Rendering strategy:
 *   1. Render the sky once per eye into the eye's half of the viewport, offsetting the
 *      observer's position by +/- IPD/2 along the eye axis and applying the predicted
 *      head orientation to the view direction.
 *   2. Composite both eyes to the screen with the inverse lens distortion applied, so
 *      the Cardboard lenses present a geometrically correct image.
 *
 * Comfort decisions baked in here (evidence in notes/VR-COMFORT-RESEARCH.md):
 *   - FOV is FIXED. It never breathes with frame rate or movement; a pumping FOV is a
 *     known discomfort source and would also break the lens distortion mapping.
 *   - The vignette is applied ONLY during app-initiated view motion (scripted pans,
 *     goto-object animation, snap turns). It is explicitly NOT applied during head
 *     rotation: the research notes that vignetting a head-driven view change can make
 *     sickness worse, because the vignette conflicts with the rotation itself.
 *   - Snap turn (default 30 degrees) is offered for controller-driven yaw instead of
 *     smooth panning; snap turning measured ~40-50% less sickness than smooth turning.
 *   - The landscape/horizon stays enabled by default: it is a rest frame, a stable
 *     visual anchor that measurably delays symptom onset. It is comfort equipment,
 *     not decoration.
 */

#ifndef STELCARDBOARDRENDERER_HPP
#define STELCARDBOARDRENDERER_HPP

#include "StelModule.hpp"
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSize>

class StelCardboardHeadTracking;
class QOpenGLFunctions;

//! Cardboard stereoscopic renderer and head-tracking host.
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

	//! Enable/disable stereo output. Disabled leaves normal mono rendering untouched.
	bool isStereoEnabled() const { return stereoEnabled; }
	void setStereoEnabled(bool b);

	//! Physical lens/screen description. These are the values that must match the actual
	//! viewer, because they drive both the eye offsets and the distortion correction;
	//! a mismatch shows up as eye strain and headaches independently of nausea.
	void setInterPupillaryDistance(double mm);
	double getInterPupillaryDistance() const { return ipdMm; }

	//! Comfort vignette during app-initiated motion only. Strength 0..1.
	void setVignetteStrength(float s);
	float getVignetteStrength() const { return vignetteStrength; }

	//! Snap-turn angle in degrees for controller-driven yaw (0 disables snap turn).
	void setSnapTurnAngle(double deg) { snapTurnAngle = deg; }
	double getSnapTurnAngle() const { return snapTurnAngle; }

	//! Rotate the view by one snap step. Uses discrete rotation, never a smooth pan,
	//! because continuous virtual rotation is the strongest sickness driver there is.
	void snapTurn(bool toLeft);

	//! Access the tracker (diagnostics overlay, tests).
	StelCardboardHeadTracking* getHeadTracking() const { return headTracking; }

	//! Human-readable status line for the diagnostics overlay.
	QString getStatusText() const;

private:
	void initializeGL();
	void drawEye(StelCore* core, int eyeIndex);
	void compositeToScreen();
	bool ensureGlResources();

	StelCardboardHeadTracking* headTracking = nullptr;

	bool stereoEnabled = true;
	bool glReady       = false;

	double ipdMm           = 64.0; //!< average adult IPD; the classic Cardboard value
	float vignetteStrength = 0.0f; //!< 0 by default: head rotation gets no vignette
	double snapTurnAngle   = 30.0; //!< degrees; research suggests 30-45

	QSize screenSize;

	// Distortion composite pass
	QOpenGLShaderProgram* distortionProgram = nullptr;
	QOpenGLVertexArrayObject* quadVao       = nullptr;
	QOpenGLBuffer* quadVbo                  = nullptr;

	//! Cardboard lens distortion coefficients (brown-conrady k1/k2). Defaults match the
	//! widely used Cardboard v1-style lens; these must be tuned per viewer.
	float lensK1 = -0.22f;
	float lensK2 = 0.0f;
};

#endif // STELCARDBOARDRENDERER_HPP
