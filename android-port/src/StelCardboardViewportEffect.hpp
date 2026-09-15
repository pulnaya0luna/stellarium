/*
 * Stellarium Cardboard port — stereoscopic viewport effect.
 *
 * This is the class that actually turns a Stellarium frame into a Cardboard split-screen
 * pair. It plugs into upstream's existing post-process hook, StelViewportEffect: the sky
 * is drawn into an offscreen buffer, and paintViewportBuffer() is the single place where
 * that buffer reaches the screen. Compositing here means the whole engine is untouched —
 * no changes to StelPainter, StelProjector, or any draw module.
 *
 * WHAT IT DRAWS
 * -------------
 *   +---------------------+---------------------+
 *   |                     |                     |
 *   |      left eye       |      right eye      |
 *   |   (barrel-distorted)|  (barrel-distorted) |
 *   |                     |                     |
 *   +---------------------+---------------------+
 *
 * The source buffer holds the two eyes side by side (left half, right half), rendered by
 * StelCardboardRenderer with the per-eye viewport and IPD offset already applied. This
 * effect samples that buffer with the inverse of the lens distortion, so the physical
 * Cardboard lenses re-distort it into a geometrically correct view.
 *
 * WHY THE DISTORTION MATTERS FOR COMFORT
 * --------------------------------------
 * A wrong distortion mapping is not merely ugly: the eyes are presented a subtly warped
 * world that they must fight, which produces eye strain and headache independently of any
 * nausea. The lens coefficients (k1/k2) must match the actual viewer.
 *
 * The vignette uniform exists but is applied ONLY when the app itself moves the view.
 * Research note (see notes/VR-COMFORT-RESEARCH.md): vignetting a head-driven view change
 * can make sickness WORSE, because the vignette conflicts with the rotation rather than
 * damping locomotion-driven optical flow. Head rotation therefore gets uVignette = 0.
 */

#ifndef STELCARDBOARDVIEWPORTEFFECT_HPP
#define STELCARDBOARDVIEWPORTEFFECT_HPP

#include "StelViewportEffect.hpp"
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

//! Cardboard split-screen stereo compositor with lens distortion correction.
class StelCardboardViewportEffect : public StelViewportEffect
{
public:
	//! @param screenW/screenH the full display size in pixels.
	//! Lens coefficients and vignette are read from the app's config (cardboard/*), so
	//! this stays constructible from a single-line hook in StelApp with no coupling.
	StelCardboardViewportEffect(int screenW, int screenH);
	~StelCardboardViewportEffect() override;

	QString getName() const override { return QStringLiteral("cardboardStereo"); }

	//! Draw the offscreen buffer to the screen as a distorted stereo pair.
	void paintViewportBuffer(const QOpenGLFramebufferObject* buf) const override;

	//! Map a screen point through the lens distortion (used for picking/UI alignment).
	void distortXY(qreal& x, qreal& y) const override;

	//! Comfort vignette strength, 0..1. Applied only for app-initiated view motion;
	//! leave at 0 during head rotation (see the header comment).
	void setVignette(float v) { vignette = qBound(0.0f, v, 1.0f); }
	float getVignette() const { return vignette; }

	//! Update the lens coefficients (per-viewer tuning).
	void setLensCoefficients(float k1, float k2)
	{
		lensK1 = k1;
		lensK2 = k2;
	}

private:
	void setupShaders();
	void setupBuffers();
	void bindVAO() const;
	void releaseVAO() const;

	//! Byte offset of the index block within the VBO, for per-row draws.
	int indexByteOffset = 0;

	int screenW;
	int screenH;
	//! Brown-Conrady radial coefficients. Defaults describe a generic Cardboard v1-class
	//! viewer; overridden from config (cardboard/lens_k1, cardboard/lens_k2).
	float lensK1 = -0.22f;
	float lensK2 = 0.0f;
	//! Comfort vignette strength, 0..1. Default 0: see the header comment for why the
	//! vignette must NOT be applied during head rotation.
	float vignette = 0.0f;
	//! Radius of the lens's visible circle, in half-height normalised units. Outside it
	//! the display is black; this is standard Cardboard masking and it is also what keeps
	//! the distortion model inside its valid range.
	float lensRadius = 1.0f;

	//! Geometry: a grid in screen space, each vertex carrying the source texcoord that
	//! must be sampled to pre-compensate the lens distortion. A grid (rather than a
	//! single quad) is required because the distortion is non-linear: interpolating
	//! across one quad would leave visible error at the edges, which reads as warping.
	int gridX = 0;
	int gridY = 0;

	std::unique_ptr<QOpenGLShaderProgram> shaderProgram;
	std::unique_ptr<QOpenGLVertexArrayObject> vao;
	std::unique_ptr<QOpenGLBuffer> vbo;

	struct ShaderVars
	{
		int projectionMatrix = -1;
		int texture          = -1;
		int vignette         = -1;
		int lensRadius       = -1;
	} shaderVars;

	int vboVertexOffset   = 0;
	int vboTexCoordOffset = 0;
	int vboIdealOffset    = 0;
};

#endif // STELCARDBOARDVIEWPORTEFFECT_HPP
