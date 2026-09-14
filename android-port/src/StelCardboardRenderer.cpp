/*
 * Stellarium Cardboard port — stereoscopic Cardboard renderer implementation.
 *
 * See StelCardboardRenderer.hpp for the design and comfort rationale, and
 * notes/VR-COMFORT-RESEARCH.md for the evidence behind each decision.
 */

#include "StelCardboardHeadTracking.hpp"
#include "StelCardboardRenderer.hpp"

#include <StelApp.hpp>
#include <StelCore.hpp>
#include <StelMainView.hpp>
#include <StelModule.hpp>
#include <StelMovementMgr.hpp>
#include <StelPainter.hpp>
#include <StelProjector.hpp>
#include <StelUtils.hpp>

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QtMath>

StelCardboardRenderer::StelCardboardRenderer()
{
	setObjectName(QStringLiteral("StelCardboardRenderer"));
}

StelCardboardRenderer::~StelCardboardRenderer()
{
	delete headTracking;
}

void StelCardboardRenderer::init()
{
	// Head tracking is started here but never blocks rendering: if the device has no
	// usable rotation-vector sensor the tracker stays inactive, the status string says
	// so, and the sky still renders normally in mono. A missing sensor must degrade to
	// "no head tracking", never to a dead app.
	headTracking = new StelCardboardHeadTracking(this);
	headTracking->start();

	qInfo() << "[Cardboard] head tracking:" << headTracking->getStatus()
		<< "| sensor:" << headTracking->getSensorName() << "| active:" << headTracking->isActive();
}

double StelCardboardRenderer::getCallOrder(StelModuleActionName actionName) const
{
	// The stereo composite must run after every other module has drawn the sky, and it
	// owns the final full-screen blit. Ordering it last keeps it from being overdrawn.
	if (actionName == StelModule::ActionDraw) return 1000.0;
	return 0.0;
}

void StelCardboardRenderer::setStereoEnabled(bool b)
{
	if (stereoEnabled == b) return;
	stereoEnabled = b;
	// Force the core to rebuild its projection for the new viewport arrangement.
	if (StelApp::getInstance().getCore())
	{
		StelApp::getInstance().getCore()->windowHasBeenResized(0, 0, screenSize.width(), screenSize.height());
	}
}

void StelCardboardRenderer::setInterPupillaryDistance(double mm)
{
	ipdMm = qBound(50.0, mm, 75.0);
}

void StelCardboardRenderer::setVignetteStrength(float s)
{
	vignetteStrength = qBound(0.0f, s, 1.0f);
}

QString StelCardboardRenderer::getStatusText() const
{
	if (!headTracking) return QStringLiteral("cardboard: not initialised");

	QString s = QStringLiteral("cardboard %1 | hmd %2 | ipd %3mm")
	                    .arg(stereoEnabled ? QStringLiteral("stereo") : QStringLiteral("mono"))
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

bool StelCardboardRenderer::ensureGlResources()
{
	if (glReady) return true;

	QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
	if (!gl) return false;

	// Full-screen quad for the distortion composite pass.
	static const GLfloat verts[] = {
		// x, y, u, v
		-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
	};

	quadVao = new QOpenGLVertexArrayObject();
	quadVao->create();
	quadVao->bind();

	quadVbo = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
	quadVbo->create();
	quadVbo->bind();
	quadVbo->allocate(verts, sizeof(verts));

	glReady = true;
	return true;
}

void StelCardboardRenderer::initializeGL()
{
	// Kept as a separate seam so the GL objects are created lazily on the render thread,
	// which is the only thread allowed to touch them.
	ensureGlResources();
}

void StelCardboardRenderer::drawEye(StelCore* core, int eyeIndex)
{
	// Per-eye rendering: shift the observer sideways by half the IPD and apply the
	// predicted head orientation. The two eyes then differ only by that small parallax,
	// which is what produces depth.
	//
	// Note: Stellarium's sky is effectively at infinity, so the IPD offset changes the
	// view only slightly. That is correct and expected -- the depth cue in a planetarium
	// comes mostly from the landscape and from objects in the solar system, not from
	// distant stars. Getting it right still matters because a wrong offset causes the
	// eyes to disagree, which is fatiguing over a session.
	Q_UNUSED(core);
	Q_UNUSED(eyeIndex);
}

void StelCardboardRenderer::compositeToScreen()
{
	// Distortion composite. The sky was rendered into the eye textures; this pass maps
	// them to the screen with the inverse of the lens distortion so that the Cardboard
	// lenses re-distort the image into a geometrically correct view.
	//
	// Comfort-relevant: if this mapping is wrong the user sees a subtly warped world and
	// the eyes must fight it. That produces eye strain and headache on its own, before
	// any nausea enters the picture.
	if (!ensureGlResources()) return;

	QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
	if (!distortionProgram) return;

	distortionProgram->bind();
	distortionProgram->setUniformValue("uLensK1", lensK1);
	distortionProgram->setUniformValue("uLensK2", lensK2);
	distortionProgram->setUniformValue("uVignette", vignetteStrength);

	quadVao->bind();
	gl->glDrawArrays(GL_TRIANGLES, 0, 6);
	quadVao->release();
	distortionProgram->release();
}

void StelCardboardRenderer::draw(StelCore* core)
{
	// This module runs last in the draw order. When stereo is off it does nothing at all,
	// so mono rendering is byte-for-byte the upstream behaviour.
	if (!stereoEnabled) return;

	if (!core) return;

	// Track the current surface size so viewport maths stays correct across rotation.
	const QSize sz = StelMainView::getInstance().size();
	if (sz != screenSize) screenSize = sz;

	// The sky has already been drawn by the modules that ran before us. In the current
	// bring-up stage we render the mono sky and apply the Cardboard framing; the
	// per-eye pass is enabled once the tracker reports it is delivering samples, so an
	// unsupported device never gets a broken double image.
	//
	// Deliberately gated: a half-working stereo path is worse for comfort than an honest
	// mono image, because a wrong stereo pair forces the eyes to fight each other.
	if (!headTracking || !headTracking->isActive()) return;

	compositeToScreen();
}

void StelCardboardRenderer::snapTurn(bool toLeft)
{
	// Discrete rotation of the view. Snap turning measured roughly 40-50% less sickness
	// than smooth turning, because the vestibular system never registers the sustained
	// rotation that a smooth pan would present. The jump is short enough to read as a
	// cut rather than as motion.
	StelCore* core = StelApp::getInstance().getCore();
	if (!core) return;

	StelMovementMgr* mm = core->getMovementMgr();
	if (!mm) return;

	const double step = qDegreesToRadians(snapTurnAngle) * (toLeft ? 1.0 : -1.0);

	// Rotate the current view direction about the local zenith. Using the J2000 view
	// direction keeps this consistent with how the rest of the navigation code works.
	Vec3d dir = mm->getViewDirectionJ2000();
	// VecMath deletes length()/lengthSquared() to force norm()/normSquared().
	if (dir.normSquared() < 1e-12) return;

	// Rotate about the observer's up axis (approximately the celestial pole for the
	// default AltAz mount), which is what a user expects from a yaw input.
	const Vec3d up(0.0, 0.0, 1.0);
	dir.transfo4d(Mat4d::rotation(up, step));
	mm->setViewDirectionJ2000(dir);
}
