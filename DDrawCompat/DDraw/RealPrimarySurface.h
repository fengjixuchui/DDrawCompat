#pragma once

#include <ddraw.h>

#include <Common/CompatPtr.h>
#include <Common/CompatRef.h>

namespace DDraw
{
	class Surface;

	class RealPrimarySurface
	{
	public:
		template <typename DirectDraw>
		static HRESULT create(CompatRef<DirectDraw> dd);

		static HRESULT flip(CompatPtr<IDirectDrawSurface7> surfaceTargetOverride, DWORD flags);
		static void flush();
		static HRESULT getGammaRamp(DDGAMMARAMP* rampData);
		static CompatWeakPtr<IDirectDrawSurface7> getSurface();
		static void init();
		static bool isFullScreen();
		static bool isLost();
		static void release();
		static void removeUpdateThread();
		static HRESULT restore();
		static void scheduleUpdate();
		static HRESULT setGammaRamp(DDGAMMARAMP* rampData);
		static void update();
		static bool waitForFlip(Surface* surface, bool wait = true);
	};
}
