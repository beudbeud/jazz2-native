#include "LibretroApplication.h"

#if defined(WITH_LIBRETRO)

#include "LibretroGfxDevice.h"
#include "LibretroInputManager.h"
#include "../../IAppEventHandler.h"
#include "../../Threading/Thread.h"

namespace nCine
{
	Application& theApplication()
	{
		static Backends::LibretroApplication instance;
		return instance;
	}
}

namespace nCine::Backends
{
	retro_environment_t LibretroApplication::EnvironmentCallback = nullptr;
	retro_video_refresh_t LibretroApplication::VideoRefreshCallback = nullptr;
	retro_input_poll_t LibretroApplication::InputPollCallback = nullptr;
	retro_input_state_t LibretroApplication::InputStateCallback = nullptr;
	bool LibretroApplication::IsInsideFrame = false;

	LibretroApplication& theLibretroApplication()
	{
		return static_cast<LibretroApplication&>(theApplication());
	}

	void LibretroApplication::ShowMessage(const char* text)
	{
		if (EnvironmentCallback == nullptr) {
			return;
		}

		retro_message message = {};
		message.msg = text;
		message.frames = 180;
		EnvironmentCallback(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}

	bool LibretroApplication::Init(CreateAppEventHandlerDelegate createAppEventHandler)
	{
		PreInitCommon(createAppEventHandler());
		if (_shouldQuit) {
			return false;
		}

		// Pacing is normally the frontend's job (vsync or audio sync), but the engine steps a
		// fixed 1/fps per retro_run, so an unthrottled frontend would fast-forward the game:
		// RunFrame() keeps a backstop of its own, bypassed automatically while the frontend
		// reports fast-forwarding (see retro_run). The engine's limiter (Application::Step())
		// cannot be that backstop: it measures from the start of the CURRENT frame, so it padded
		// every retro_run to a full period, and the frontend's own vsync/audio wait then came on
		// top of it - about 50 fps in an idle level on a Raspberry Pi 5, less wherever the GPU
		// had work the limiter had already refused to overlap with. It stays off whatever the
		// "Max FPS" preference says
		_appCfg.frameLimit = 0;
		SetFrameLimiter(true);
		_appCfg.withVSync = false;

		IGfxDevice::ContextInfo contextInfo(_appCfg);
		DisplayMode displayMode(8, 8, 8, 8, 24, 8, DisplayMode::DoubleBuffering::Enabled, DisplayMode::VSync::Disabled);
		std::int32_t width = (_appCfg.resolution.X > 0 ? _appCfg.resolution.X : 720);
		std::int32_t height = (_appCfg.resolution.Y > 0 ? _appCfg.resolution.Y : 405);
		const IGfxDevice::WindowMode windowMode(width, height, 0, 0, false, false, false);

		_gfxDevice = std::make_unique<LibretroGfxDevice>(windowMode, contextInfo, displayMode);
		_inputManager = std::make_unique<LibretroInputManager>();

		InitCommon();

		SetAutoSuspension(false);
		SetFocus(true);
		static_cast<LibretroInputManager&>(*_inputManager).connectJoystick();
		return !_shouldQuit;
	}

	void LibretroApplication::RunFrame()
	{
		if (_frameLimiterEnabled) {
			// Measured from the start of the PREVIOUS run, so whatever the frontend waited on between the
			// two (vsync, audio sync, its frame delay) counts toward the period instead of adding to it - a
			// throttled frontend never arrives early enough to sleep here. The tolerance is for a display
			// running a hair above its nominal rate, which a limiter at exactly the period would hold back
			// to a dropped frame every few seconds; an unthrottled frontend is still caught, it would be
			// here many times too early. A coarse sleep covers the bulk, Sleep(0) yields away the last bit
			constexpr float ToleranceSecs = 0.001f;
			const float period = 1.0f / (float)LibretroGfxDevice::GetTargetFps() - ToleranceSecs;
			const float remaining = period - _lastRunStart.secondsSince();
			if (remaining > 0.002f) {
				Thread::Sleep((std::uint32_t)((remaining - 0.002f) * 1000.0f));
			}
			while (_lastRunStart.secondsSince() < period) {
				Thread::Sleep(0);
			}
		}
		_lastRunStart = TimeStamp::now();

		static_cast<LibretroGfxDevice&>(*_gfxDevice).beginFrame();
		static_cast<LibretroInputManager&>(*_inputManager).processFrame();
		Step();
	}

	void LibretroApplication::SetFrameLimiter(bool enabled)
	{
		_frameLimiterEnabled = enabled;
	}

	void LibretroApplication::Shutdown()
	{
		ShutdownCommon();
	}

	bool LibretroApplication::SaveState(Death::IO::Stream& dest)
	{
		return (_appEventHandler != nullptr && _appEventHandler->OnSaveState(dest));
	}

	bool LibretroApplication::LoadState(std::shared_ptr<Death::IO::Stream> src)
	{
		return (_appEventHandler != nullptr && _appEventHandler->OnLoadState(std::move(src)));
	}
}

#endif
