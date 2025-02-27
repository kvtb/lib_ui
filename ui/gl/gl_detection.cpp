// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/gl/gl_detection.h"

#include "ui/gl/gl_shader.h"
#include "ui/integration.h"
#include "base/debug_log.h"

#include <QtCore/QSet>
#include <QtCore/QFile>
#include <QtGui/QWindow>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtWidgets/QOpenGLWidget>

#ifdef Q_OS_WIN
#include <QtGui/QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <EGL/egl.h>
#endif // Q_OS_WIN

#define LOG_ONCE(x) [[maybe_unused]] static auto logged = [&] { LOG(x); return true; }();

namespace Ui::GL {
namespace {

bool ForceDisabled/* = false*/;

#ifdef Q_OS_WIN
ANGLE ResolvedANGLE = ANGLE::Auto;
#endif // Q_OS_WIN

void CrashCheckStart() {
	auto f = QFile(Integration::Instance().openglCheckFilePath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write("1", 1);
		f.close();
	}
}

} // namespace

Capabilities CheckCapabilities(QWidget *widget) {
	if (ForceDisabled) {
		LOG_ONCE(("OpenGL: Force-disabled."));
		return {};
	}

	[[maybe_unused]] static const auto BugListInited = [] {
		if (!QFile::exists(":/misc/gpu_driver_bug_list.json")) {
			return false;
		}
		LOG(("OpenGL: Using custom 'gpu_driver_bug_list.json'."));
		qputenv("QT_OPENGL_BUGLIST", ":/misc/gpu_driver_bug_list.json");
		return true;
	}();

	auto format = QSurfaceFormat();
	if (widget) {
		if (!widget->window()->windowHandle()) {
			widget->window()->createWinId();
		}
		if (!widget->window()->windowHandle()) {
			LOG(("OpenGL: Could not create window for widget."));
			return {};
		}
		if (!widget->window()->windowHandle()->supportsOpenGL()) {
			LOG_ONCE(("OpenGL: Not supported for window."));
			return {};
		}
		format = widget->window()->windowHandle()->format();
		format.setAlphaBufferSize(8);
		widget->window()->windowHandle()->setFormat(format);
	} else {
		format.setAlphaBufferSize(8);
	}
	auto tester = QOpenGLWidget(widget);
	tester.setFormat(format);

	CrashCheckStart();
	tester.grabFramebuffer(); // Force initialize().
	CrashCheckFinish();

	if (!tester.window()->windowHandle()) {
		tester.window()->createWinId();
	}
	const auto context = tester.context();
	if (!context
		|| !context->isValid()/*
		// This check doesn't work for a widget with WA_NativeWindow.
		|| !context->makeCurrent(tester.window()->windowHandle())*/) {
		LOG_ONCE(("OpenGL: Could not create widget in a window."));
		return {};
	}
	const auto functions = context->functions();
	using Feature = QOpenGLFunctions;
	if (!functions->hasOpenGLFeature(Feature::NPOTTextures)) {
		LOG_ONCE(("OpenGL: NPOT textures not supported."));
		return {};
	} else if (!functions->hasOpenGLFeature(Feature::Framebuffers)) {
		LOG_ONCE(("OpenGL: Framebuffers not supported."));
		return {};
	} else if (!functions->hasOpenGLFeature(Feature::Shaders)) {
		LOG_ONCE(("OpenGL: Shaders not supported."));
		return {};
	}
	{
		auto program = QOpenGLShaderProgram();
		LinkProgram(
			&program,
			VertexShader({
				VertexViewportTransform(),
				VertexPassTextureCoord(),
			}),
			FragmentShader({
				FragmentSampleARGB32Texture(),
			}));
		if (!program.isLinked()) {
			LOG_ONCE(("OpenGL: Could not link simple shader."));
			return {};
		}
	}

	const auto supported = context->format();
	switch (supported.profile()) {
	case QSurfaceFormat::NoProfile: {
		if (supported.renderableType() == QSurfaceFormat::OpenGLES) {
			LOG_ONCE(("OpenGL Profile: OpenGLES."));
		} else {
			LOG_ONCE(("OpenGL Profile: None."));
			return {};
		}
	} break;
	case QSurfaceFormat::CoreProfile: {
		LOG_ONCE(("OpenGL Profile: Core."));
	} break;
	case QSurfaceFormat::CompatibilityProfile: {
		LOG_ONCE(("OpenGL Profile: Compatibility."));
	} break;
	}

	[[maybe_unused]] static const auto extensionsLogged = [&] {
		const auto renderer = reinterpret_cast<const char*>(
			functions->glGetString(GL_RENDERER));
		LOG(("OpenGL Renderer: %1").arg(renderer ? renderer : "[nullptr]"));
		const auto vendor = reinterpret_cast<const char*>(
			functions->glGetString(GL_VENDOR));
		LOG(("OpenGL Vendor: %1").arg(vendor ? vendor : "[nullptr]"));
		const auto version = reinterpret_cast<const char*>(
			functions->glGetString(GL_VERSION));
		LOG(("OpenGL Version: %1").arg(version ? version : "[nullptr]"));
		auto list = QStringList();
		for (const auto &extension : context->extensions()) {
			list.append(QString::fromLatin1(extension));
		}
		LOG(("OpenGL Extensions: %1").arg(list.join(", ")));

#ifdef Q_OS_WIN
		auto egllist = QStringList();
		for (const auto &extension : EGLExtensions(context)) {
			egllist.append(QString::fromLatin1(extension));
		}
		LOG(("EGL Extensions: %1").arg(egllist.join(", ")));
#endif // Q_OS_WIN

		return true;
	}();

	const auto version = u"%1.%2"_q
		.arg(supported.majorVersion())
		.arg(supported.majorVersion());
	auto result = Capabilities{ .supported = true };
	if (supported.alphaBufferSize() >= 8) {
		result.transparency = true;
		LOG_ONCE(("OpenGL: QOpenGLContext created, version: %1."
			).arg(version));
	} else {
		LOG_ONCE(("OpenGL: QOpenGLContext without alpha created, version: %1"
			).arg(version));
	}
	return result;
}

bool LastCrashCheckFailed() {
	return QFile::exists(Integration::Instance().openglCheckFilePath());
}

void CrashCheckFinish() {
	QFile::remove(Integration::Instance().openglCheckFilePath());
}

void ForceDisable(bool disable) {
	ForceDisabled = disable;
}

#ifdef Q_OS_WIN

void ConfigureANGLE() {
	qunsetenv("DESKTOP_APP_QT_ANGLE_PLATFORM");
	const auto path = Ui::Integration::Instance().angleBackendFilePath();
	if (path.isEmpty()) {
		return;
	}
	auto f = QFile(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	auto bytes = f.read(32);
	const auto check = [&](const char *backend, ANGLE angle) {
		if (bytes.startsWith(backend)) {
			ResolvedANGLE = angle;
			qputenv("DESKTOP_APP_QT_ANGLE_PLATFORM", backend);
		}
	};
	check("gl", ANGLE::OpenGL);
	check("d3d9", ANGLE::D3D9);
	check("d3d11", ANGLE::D3D11);
	check("d3d11on12", ANGLE::D3D11on12);
	if (ResolvedANGLE == ANGLE::Auto) {
		LOG(("ANGLE Warning: Unknown backend: %1"
			).arg(QString::fromUtf8(bytes)));
	}
}

void ChangeANGLE(ANGLE backend) {
	const auto path = Ui::Integration::Instance().angleBackendFilePath();
	const auto write = [&](QByteArray backend) {
		auto f = QFile(path);
		if (!f.open(QIODevice::WriteOnly)) {
			LOG(("ANGLE Warning: Could not write to %1.").arg(path));
			return;
		}
		f.write(backend);
	};
	switch (backend) {
	case ANGLE::Auto: QFile(path).remove(); break;
	case ANGLE::D3D9: write("d3d9"); break;
	case ANGLE::D3D11: write("d3d11"); break;
	case ANGLE::D3D11on12: write("d3d11on12"); break;
	case ANGLE::OpenGL: write("gl"); break;
	default: Unexpected("ANGLE backend value.");
	}
}

ANGLE CurrentANGLE() {
	return ResolvedANGLE;
}

QList<QByteArray> EGLExtensions(not_null<QOpenGLContext*> context) {
	const auto native = QGuiApplication::platformNativeInterface();
	Assert(native != nullptr);

	const auto display = static_cast<EGLDisplay>(
		native->nativeResourceForContext(
			QByteArrayLiteral("egldisplay"),
			context));
	return display
		? QByteArray(eglQueryString(display, EGL_EXTENSIONS)).split(' ')
		: QList<QByteArray>();
}

#endif // Q_OS_WIN

} // namespace Ui::GL
