/*
 * Stellarium Cardboard port — stereoscopic viewport effect implementation.
 * See StelCardboardViewportEffect.hpp for the design and comfort rationale, and
 * notes/VR-COMFORT-RESEARCH.md for the evidence behind each decision.
 */

#include "StelCardboardViewportEffect.hpp"
#include "StelCardboardDistortion.hpp"

#include <StelApp.hpp>
#include <StelCore.hpp>
#include <StelOpenGL.hpp>
#include <StelPainter.hpp>
#include <StelProjector.hpp>

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QtMath>

//! Grid resolution for the distortion mesh. The distortion is non-linear, so it is
//! evaluated per-vertex and interpolated across triangles. Too coarse and the warp shows
//! visible faceting at the edges (which reads as geometric error to the eye); too fine and
//! we waste vertex work on a phone GPU. 32x32 per eye is a good balance for the gentle
//! curvature a Cardboard lens needs.
static constexpr int GRID_X = 32;
static constexpr int GRID_Y = 32;

StelCardboardViewportEffect::StelCardboardViewportEffect(int screenW, int screenH)
	: screenW(qMax(2, screenW))
	, screenH(qMax(2, screenH))
{
	// Read the viewer parameters from config so this class has no dependency on the rest
	// of the port. Defaults describe a generic Cardboard v1-class viewer.
	QSettings* conf = StelApp::getInstance().getSettings();
	if (conf)
	{
		lensK1     = conf->value("cardboard/lens_k1", -0.22f).toFloat();
		lensK2     = conf->value("cardboard/lens_k2", 0.0f).toFloat();
		vignette   = qBound(0.0f, conf->value("cardboard/vignette", 0.0f).toFloat(), 1.0f);
		lensRadius = qBound(0.2f, conf->value("cardboard/lens_radius", 1.0f).toFloat(), 2.0f);
	}

	// The size handed in is in DEVICE-INDEPENDENT pixels; the framebuffer is in real
	// device pixels. Upstream's fisheye distorter does the same conversion. Without it
	// the mesh geometry is built against a surface smaller than the real one by the
	// screen density factor (2.6x on a 420 dpi phone), so anything expressed in absolute
	// pixels -- the IPD offset especially -- would be wrong.
	if (StelCore* core = StelApp::getInstance().getCore())
	{
		const double dpp = core->getCurrentStelProjectorParams().devicePixelsPerPixel;
		if (dpp > 0.0 && dpp != 1.0)
		{
			screenW = qMax(2, int(screenW * dpp));
			screenH = qMax(2, int(screenH * dpp));
		}
	}

	// GL objects must be created on a thread with a current context.
	StelApp::getInstance().ensureGLContextCurrent();
	setupShaders();
	setupBuffers();
}

StelCardboardViewportEffect::~StelCardboardViewportEffect()
{
	StelApp::getInstance().ensureGLContextCurrent();
	// The unique_ptrs release their GL objects here, with the context current.
}

void StelCardboardViewportEffect::setupShaders()
{
	QOpenGLShader vsh(QOpenGLShader::Vertex);
	// globalShaderPrefix supplies the correct dialect per platform: "#version 300 es" on
	// GLES (Android), "#version 330" on desktop, plus ATTRIBUTE/VARYING and precision
	// qualifiers. Using it means one shader source serves both, which keeps the port
	// testable on the host.
	const auto vsrc = StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) +
	                  "ATTRIBUTE highp vec3 vertex;\n"
	                  "ATTRIBUTE mediump vec2 texCoord;\n"
	                  "ATTRIBUTE mediump vec2 idealPos;\n"
	                  "VARYING mediump vec2 texc;\n"
	                  "VARYING mediump vec2 ideal;\n"
	                  "void main()\n"
	                  "{\n"
	                  // Vertex positions arrive already in normalised device coordinates, so no
	                  // projection matrix is needed: the mesh is laid out in screen space directly.
	                  "    gl_Position = vec4(vertex.xy, 0.0, 1.0);\n"
	                  "    texc = texCoord;\n"
	                  "    ideal = idealPos;\n"
	                  "}\n";
	vsh.compileSourceCode(vsrc);
	if (!vsh.log().isEmpty()) qWarning().noquote() << "[Cardboard] vertex shader log:" << vsh.log();

	QOpenGLShader fsh(QOpenGLShader::Fragment);
	// Two jobs:
	//
	//  1. MASK the lens circle. Anything outside it is black -- that is standard Cardboard
	//     practice (it hides the physical edge of the lens) and it is also what keeps the
	//     distortion model honest. The eye's rectangular viewport has corners at
	//     r = sqrt(aspect^2 + 1), about 1.6 for a landscape phone, which is well past the
	//     Brown-Conrady model's monotonic limit (~1.23 for k1 = -0.22). Rendering that
	//     region would clamp every such sample to one ring and smear it radially across
	//     the periphery -- which is exactly what the first device test showed. Inside the
	//     lens circle (r <= 1) the model is valid, so nothing needs to be clamped.
	//
	//  2. The vignette, centred on the EYE's optical axis, driven by the ideal eye-local
	//     position rather than the sampled texcoord -- the texcoord spans the whole buffer,
	//     so using it would centre the vignette on the screen between the eyes.
	const auto fsrc =
		StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
		"VARYING mediump vec2 texc;\n"
		"VARYING mediump vec2 ideal;\n"
		"uniform sampler2D tex;\n"
		"uniform mediump float uVignette;\n"
		"uniform mediump float uLensRadius;\n"
		"void main()\n"
		"{\n"
		"    mediump float d = length(ideal);\n"
		"    if (d > uLensRadius)\n"
		"    {\n"
		"        FRAG_COLOR = vec4(0.0, 0.0, 0.0, 1.0);\n"
		"        return;\n"
		"    }\n"
		"    lowp vec4 c = texture2D(tex, texc);\n"
		"    if (uVignette > 0.0)\n"
		"    {\n"
		"        mediump float v = 1.0 - uVignette * smoothstep(0.45 * uLensRadius, uLensRadius, d);\n"
		"        c.rgb *= v;\n"
		"    }\n"
		"    FRAG_COLOR = c;\n"
		"}\n";
	fsh.compileSourceCode(fsrc);
	if (!fsh.log().isEmpty()) qWarning().noquote() << "[Cardboard] fragment shader log:" << fsh.log();

	shaderProgram.reset(new QOpenGLShaderProgram(QOpenGLContext::currentContext()));
	shaderProgram->addShader(&vsh);
	shaderProgram->addShader(&fsh);
	shaderProgram->bindAttributeLocation("vertex", 0);
	shaderProgram->bindAttributeLocation("texCoord", 1);
	shaderProgram->bindAttributeLocation("idealPos", 2);
	StelPainter::linkProg(shaderProgram.get(), "cardboard stereo distortion");

	shaderVars.texture    = shaderProgram->uniformLocation("tex");
	shaderVars.vignette   = shaderProgram->uniformLocation("uVignette");
	shaderVars.lensRadius = shaderProgram->uniformLocation("uLensRadius");
}

void StelCardboardViewportEffect::setupBuffers()
{
	gridX = GRID_X;
	gridY = GRID_Y;

	const int vertsPerEye = (gridX + 1) * (gridY + 1);

	// Build one interleaved layout: [vertex.xyz][texcoord.xy][idealPos.xy] per vertex, for
	// both eyes.
	QVector<float> vertexData;
	QVector<float> texCoordData;
	QVector<float> idealData;
	vertexData.reserve(vertsPerEye * 2 * 3);
	texCoordData.reserve(vertsPerEye * 2 * 2);
	idealData.reserve(vertsPerEye * 2 * 2);

	// The scene is rendered once, full-screen, with the FOV disk centred (Stellarium sets
	// viewportFovDiameter to min(width,height)). Each eye must show that disc without
	// distortion, so both eyes sample the SAME centred region of the buffer -- a region
	// shaped like one eye's viewport, so there is no anisotropic stretch.
	//
	// Using the full buffer width instead would squash the sky horizontally by the ratio
	// of the screen width to one eye's width (2x on a landscape phone), which is a silent
	// geometric error: the image still looks like a sky, just the wrong shape.
	const int eyeHalfW    = screenW / 2;
	const int eyeH        = screenH;
	const float eyeAspect = float(eyeHalfW) / float(qMax(1, eyeH));

	for (int eye = 0; eye < 2; ++eye)
	{
		for (int j = 0; j <= gridY; ++j)
		{
			for (int i = 0; i <= gridX; ++i)
			{
				// Eye-local position, normalised by HALF-HEIGHT so the distortion is
				// radial in physical space (circular) rather than in a squashed unit
				// square. x therefore spans +/-aspect and y spans +/-1.
				const float nx = ((float(i) / gridX) * 2.0f - 1.0f) * eyeAspect;
				const float ny = (float(j) / gridY) * 2.0f - 1.0f;

				// Where this output pixel must sample from, to pre-cancel the lens warp.
				float sx = nx;
				float sy = ny;
				StelCardboard::preDistortNormalised(sx, sy, lensK1, lensK2);

				// Position in normalised device coordinates for the whole screen: the
				// left eye occupies x in [-1,0] and the right eye [0,1].
				const float ndcX = (eye == 0) ? (-1.0f + (float(i) / gridX)) : (float(i) / gridX);
				const float ndcY = ny; // ny spans -1 (bottom) .. +1 (top)
				vertexData << ndcX << ndcY << 0.0f;

				// Sample the centred region. One unit of the normalised space is half the
				// eye height in pixels, so convert through that.
				const float halfH = float(eyeH) * 0.5f;
				const float u     = 0.5f + (sx * halfH) / float(screenW);
				const float v     = 0.5f + (sy * halfH) / float(screenH);
				texCoordData << u << v;

				// The ideal (unwarped) eye-local position, in physical (half-height) units,
				// so per-eye effects such as the vignette are centred on the eye's optical
				// axis rather than on the screen centre between the eyes.
				idealData << nx << ny;
			}
		}
	}

	// Indices: one triangle strip per grid row, per eye.
	//
	// Rows are laid out as separate strips and DRAWN separately (see paintViewportBuffer).
	// Concatenating them into one GL_TRIANGLE_STRIP would join the end of each row to the
	// start of the next, creating a triangle spanning the full width of the mesh -- which
	// renders as long horizontal streaks torn across the image. That bug was visible in
	// the first device test.
	QVector<GLuint> indices;
	for (int eye = 0; eye < 2; ++eye)
	{
		const int base = eye * vertsPerEye;
		for (int j = 0; j < gridY; ++j)
		{
			for (int i = 0; i <= gridX; ++i)
			{
				indices << GLuint(base + j * (gridX + 1) + i);
				indices << GLuint(base + (j + 1) * (gridX + 1) + i);
			}
		}
	}

	vbo.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	vbo->create();
	vbo->setUsagePattern(QOpenGLBuffer::StaticDraw);
	vbo->bind();

	// Layout: all vertex positions, then all texcoords, then all ideal positions, then
	// indices.
	const int vertexBytes   = vertexData.size() * sizeof(float);
	const int texCoordBytes = texCoordData.size() * sizeof(float);
	const int idealBytes    = idealData.size() * sizeof(float);
	vboVertexOffset         = 0;
	vboTexCoordOffset       = vertexBytes;
	vboIdealOffset          = vertexBytes + texCoordBytes;
	const int indexOffset   = vertexBytes + texCoordBytes + idealBytes;
	indexByteOffset         = indexOffset;

	vbo->allocate(indexOffset + indices.size() * sizeof(GLuint));
	vbo->write(vboVertexOffset, vertexData.constData(), vertexBytes);
	vbo->write(vboTexCoordOffset, texCoordData.constData(), texCoordBytes);
	vbo->write(vboIdealOffset, idealData.constData(), idealBytes);
	vbo->write(indexOffset, indices.constData(), indices.size() * sizeof(GLuint));
	vbo->release();

	vao.reset(new QOpenGLVertexArrayObject());
	vao->create();
	vao->bind();
	vbo->bind();
	auto& gl = *QOpenGLContext::currentContext()->functions();
	gl.glEnableVertexAttribArray(0);
	gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<const void*>(uintptr_t(vboVertexOffset)));
	gl.glEnableVertexAttribArray(1);
	gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0,
	                         reinterpret_cast<const void*>(uintptr_t(vboTexCoordOffset)));
	gl.glEnableVertexAttribArray(2);
	gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, reinterpret_cast<const void*>(uintptr_t(vboIdealOffset)));
	// Element buffer binding is captured by the VAO.
	gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo->bufferId());
	vao->release();
	vbo->release();

	qInfo() << "[Cardboard] stereo mesh:" << gridX << "x" << gridY << "per eye | surface" << screenW << "x"
		<< screenH << "| eye" << (screenW / 2) << "x" << screenH << "| lens k1 =" << lensK1 << "k2 =" << lensK2;
}

void StelCardboardViewportEffect::bindVAO() const
{
	if (vao && vao->isCreated()) vao->bind();
}

void StelCardboardViewportEffect::releaseVAO() const
{
	if (vao && vao->isCreated()) vao->release();
}

void StelCardboardViewportEffect::paintViewportBuffer(const QOpenGLFramebufferObject* buf) const
{
	if (!buf || !shaderProgram) return;

	auto& gl = *QOpenGLContext::currentContext()->functions();

	// Fill the whole screen first: the distortion mesh does not cover the corners (the
	// lens never shows them), and leaving them as garbage would look like a rendering bug.
	gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	gl.glClear(GL_COLOR_BUFFER_BIT);

	// The stereo buffer holds both eyes; sample it with linear filtering so the per-eye
	// warp does not alias.
	GL(gl.glActiveTexture(GL_TEXTURE0));
	GL(gl.glBindTexture(GL_TEXTURE_2D, buf->texture()));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

	shaderProgram->bind();
	shaderProgram->setUniformValue(shaderVars.texture, 0);
	shaderProgram->setUniformValue(shaderVars.vignette, vignette);
	shaderProgram->setUniformValue(shaderVars.lensRadius, lensRadius);

	bindVAO();
	// One draw per grid row, per eye. Drawing the whole mesh as a single strip would join
	// row ends to the next row's start and streak triangles across the image; upstream's
	// own distortion effect likewise issues a draw per row.
	const int idxPerRow = (gridX + 1) * 2;
	for (int eye = 0; eye < 2; ++eye)
	{
		for (int j = 0; j < gridY; ++j)
		{
			const int idxIndex = (eye * gridY + j) * idxPerRow;
			const void* offset = reinterpret_cast<const void*>(
				uintptr_t(indexByteOffset + idxIndex * int(sizeof(GLuint))));
			GL(gl.glDrawElements(GL_TRIANGLE_STRIP, idxPerRow, GL_UNSIGNED_INT, offset));
		}
	}
	releaseVAO();
	shaderProgram->release();

	// Restore filtering, since other code may reuse this texture binding.
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL(gl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
}

void StelCardboardViewportEffect::distortXY(qreal& x, qreal& y) const
{
	// Map a screen point through the same lens warp used for rendering, so picking and
	// any UI drawn on top lines up with what the user sees.
	float nx = float((x / screenW) * 2.0 - 1.0);
	float ny = float((y / screenH) * 2.0 - 1.0);
	StelCardboard::preDistortNormalised(nx, ny, lensK1, lensK2);
	x = (qreal(nx) * 0.5 + 0.5) * screenW;
	y = (qreal(ny) * 0.5 + 0.5) * screenH;
}
