#include <unordered_map>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Gdi/AccessGuard.h>
#include <Gdi/Dc.h>
#include <Gdi/DcFunctions.h>
#include <Gdi/Font.h>
#include <Gdi/Gdi.h>
#include <Gdi/Region.h>
#include <Gdi/VirtualScreen.h>
#include <Gdi/Window.h>
#include <Win32/DisplayMode.h>

namespace
{
	class CompatDc
	{
	public:
		CompatDc(HDC dc) : m_origDc(dc), m_compatDc(Gdi::Dc::getDc(dc, false)) 
		{
		}

		CompatDc(const CompatDc&) = delete;

		CompatDc(CompatDc&& other) : m_origDc(nullptr), m_compatDc(nullptr)
		{
			std::swap(m_origDc, other.m_origDc);
			std::swap(m_compatDc, other.m_compatDc);
		}

		~CompatDc()
		{
			if (m_compatDc)
			{
				Gdi::Dc::releaseDc(m_origDc);
			}
		}

		operator HDC() const
		{
			return m_compatDc ? m_compatDc : m_origDc;
		}

	private:
		HDC m_origDc;
		HDC m_compatDc;
	};

	struct ExcludeRgnForOverlappingWindowArgs
	{
		HRGN rgn;
		HWND rootWnd;
	};

	std::unordered_map<void*, const char*> g_funcNames;
	thread_local bool g_redirectToDib = true;

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename... Params>
	HDC getDestinationDc(Params... params);

	HRGN getWindowRegion(HWND hwnd);

	BOOL WINAPI GdiDrawStream(HDC, DWORD, DWORD) { return FALSE; }
	BOOL WINAPI PolyPatBlt(HDC, DWORD, DWORD, DWORD, DWORD) { return FALSE; }

	template <typename Result, typename... Params>
	using FuncPtr = Result(WINAPI *)(Params...);

	bool hasDisplayDcArg(HDC dc)
	{
		return Gdi::isDisplayDc(dc);
	}

	template <typename T>
	bool hasDisplayDcArg(T)
	{
		return false;
	}

	template <typename T, typename... Params>
	bool hasDisplayDcArg(T t, Params... params)
	{
		return hasDisplayDcArg(t) || hasDisplayDcArg(params...);
	}

	bool lpToScreen(HWND hwnd, HDC dc, POINT& p)
	{
		LPtoDP(dc, &p, 1);
		RECT wr = {};
		GetWindowRect(hwnd, &wr);
		p.x += wr.left;
		p.y += wr.top;
		return true;
	}

	template <typename T>
	T replaceDc(T t)
	{
		return t;
	}

	CompatDc replaceDc(HDC dc)
	{
		return CompatDc(dc);
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	Result WINAPI compatGdiDcFunc(Params... params)
	{
#ifdef DEBUGLOGS
		LOG_FUNC(g_funcNames[origFunc], params...);
#else
		LOG_FUNC("", params...);
#endif

		if (hasDisplayDcArg(params...))
		{
			D3dDdi::ScopedCriticalSection lock;
			const bool isReadOnlyAccess = !hasDisplayDcArg(getDestinationDc<OrigFuncPtr, origFunc>(params...));
			Gdi::AccessGuard accessGuard(isReadOnlyAccess ? Gdi::ACCESS_READ : Gdi::ACCESS_WRITE);
			return LOG_RESULT(Compat::getOrigFuncPtr<OrigFuncPtr, origFunc>()(replaceDc(params)...));
		}

		return LOG_RESULT(Compat::getOrigFuncPtr<OrigFuncPtr, origFunc>()(params...));
	}

	template <>
	BOOL WINAPI compatGdiDcFunc<decltype(&ExtTextOutW), &ExtTextOutW>(
		HDC hdc, int x, int y, UINT options, const RECT* lprect, LPCWSTR lpString, UINT c, const INT* lpDx)
	{
		LOG_FUNC("ExtTextOutW", hdc, x, y, options, lprect, lpString, c, lpDx);

		if (hasDisplayDcArg(hdc))
		{
			HWND hwnd = CALL_ORIG_FUNC(WindowFromDC)(hdc);
			ATOM atom = static_cast<ATOM>(GetClassLong(hwnd, GCW_ATOM));
			POINT p = { x, y };
			if (Gdi::MENU_ATOM == atom)
			{
				RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
			}
			else if (GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, nullptr) &&
				lpToScreen(hwnd, hdc, p) &&
				HTMENU == SendMessage(hwnd, WM_NCHITTEST, 0, (p.y << 16) | (p.x & 0xFFFF)))
			{
				WINDOWINFO wi = {};
				GetWindowInfo(hwnd, &wi);
				Gdi::Region ncRegion(wi.rcWindow);
				ncRegion -= wi.rcClient;
				ncRegion.offset(-wi.rcClient.left, -wi.rcClient.top);
				RedrawWindow(hwnd, nullptr, ncRegion, RDW_INVALIDATE | RDW_FRAME);
			}
			else
			{
				D3dDdi::ScopedCriticalSection lock;
				Gdi::AccessGuard accessGuard(Gdi::ACCESS_WRITE);
				return LOG_RESULT(CALL_ORIG_FUNC(ExtTextOutW)(replaceDc(hdc), x, y, options, lprect, lpString, c, lpDx));
			}
		}
		else
		{
			return LOG_RESULT(CALL_ORIG_FUNC(ExtTextOutW)(hdc, x, y, options, lprect, lpString, c, lpDx));
		}

		return LOG_RESULT(TRUE);
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	Result WINAPI compatGdiTextDcFunc(HDC dc, Params... params)
	{
		Gdi::Font::Mapper fontMapper(dc);
		return compatGdiDcFunc<OrigFuncPtr, origFunc, Result>(dc, params...);
	}

	HBITMAP WINAPI createCompatibleBitmap(HDC hdc, int cx, int cy)
	{
		LOG_FUNC("CreateCompatibleBitmap", hdc, cx, cy);
		if (g_redirectToDib && Gdi::isDisplayDc(hdc))
		{
			return LOG_RESULT(Gdi::VirtualScreen::createOffScreenDib(cx, cy));
		}
		return LOG_RESULT(CALL_ORIG_FUNC(CreateCompatibleBitmap)(hdc, cx, cy));
	}

	HBITMAP WINAPI createDIBitmap(HDC hdc, const BITMAPINFOHEADER* lpbmih, DWORD fdwInit,
		const void* lpbInit, const BITMAPINFO* lpbmi, UINT fuUsage)
	{
		LOG_FUNC("CreateDIBitmap", hdc, lpbmih, fdwInit, lpbInit, lpbmi, fuUsage);
		const DWORD CBM_CREATDIB = 2;
		if (g_redirectToDib && !(fdwInit & CBM_CREATDIB) && lpbmih && Gdi::isDisplayDc(hdc))
		{
			HBITMAP bitmap = Gdi::VirtualScreen::createOffScreenDib(lpbmi->bmiHeader.biWidth, lpbmi->bmiHeader.biHeight);
			if (bitmap && lpbInit && lpbmi)
			{
				SetDIBits(hdc, bitmap, 0, std::abs(lpbmi->bmiHeader.biHeight), lpbInit, lpbmi, fuUsage);
			}
			return LOG_RESULT(bitmap);
		}
		return LOG_RESULT(CALL_ORIG_FUNC(CreateDIBitmap)(hdc, lpbmih, fdwInit, lpbInit, lpbmi, fuUsage));
	}

	HBITMAP WINAPI createDiscardableBitmap(HDC hdc, int nWidth, int nHeight)
	{
		LOG_FUNC("CreateDiscardableBitmap", hdc, nWidth, nHeight);
		if (g_redirectToDib && Gdi::isDisplayDc(hdc))
		{
			return LOG_RESULT(Gdi::VirtualScreen::createOffScreenDib(nWidth, nHeight));
		}
		return LOG_RESULT(CALL_ORIG_FUNC(createDiscardableBitmap)(hdc, nWidth, nHeight));
	}

	BOOL CALLBACK excludeRgnForOverlappingWindow(HWND hwnd, LPARAM lParam)
	{
		auto& args = *reinterpret_cast<ExcludeRgnForOverlappingWindowArgs*>(lParam);
		if (hwnd == args.rootWnd)
		{
			return FALSE;
		}

		DWORD windowPid = 0;
		GetWindowThreadProcessId(hwnd, &windowPid);
		if (GetCurrentProcessId() != windowPid ||
			!IsWindowVisible(hwnd) ||
			(CALL_ORIG_FUNC(GetWindowLongA)(hwnd, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_TRANSPARENT)))
		{
			return TRUE;
		}

		HRGN windowRgn = getWindowRegion(hwnd);
		CombineRgn(args.rgn, args.rgn, windowRgn, RGN_DIFF);
		DeleteObject(windowRgn);

		return TRUE;
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	OrigFuncPtr getCompatGdiDcFuncPtr(FuncPtr<Result, Params...>)
	{
		return &compatGdiDcFunc<OrigFuncPtr, origFunc, Result, Params...>;
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename Result, typename... Params>
	OrigFuncPtr getCompatGdiTextDcFuncPtr(FuncPtr<Result, HDC, Params...>)
	{
		return &compatGdiTextDcFunc<OrigFuncPtr, origFunc, Result, Params...>;
	}

	HDC getFirstDc()
	{
		return nullptr;
	}

	template <typename... Params>
	HDC getFirstDc(HDC dc, Params...)
	{
		return dc;
	}

	template <typename FirstParam, typename... Params>
	HDC getFirstDc(FirstParam, Params... params)
	{
		return getFirstDc(params...);
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc, typename... Params>
	HDC getDestinationDc(Params... params)
	{
		return getFirstDc(params...);
	}

	template <>
	HDC getDestinationDc<decltype(&GetDIBits), &GetDIBits>(
		HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT)
	{
		return nullptr;
	}

	template <>
	HDC getDestinationDc<decltype(&GetPixel), &GetPixel>(HDC, int, int)
	{
		return nullptr;
	}

	HRGN getWindowRegion(HWND hwnd)
	{
		RECT wr = {};
		GetWindowRect(hwnd, &wr);
		HRGN windowRgn = CreateRectRgnIndirect(&wr);
		if (ERROR != GetWindowRgn(hwnd, windowRgn))
		{
			OffsetRgn(windowRgn, wr.left, wr.top);
		}
		return windowRgn;
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc>
	void hookGdiDcFunction(const char* moduleName, const char* funcName)
	{
#ifdef DEBUGLOGS
		g_funcNames[origFunc] = funcName;
#endif

		Compat::hookFunction<OrigFuncPtr, origFunc>(
			moduleName, funcName, getCompatGdiDcFuncPtr<OrigFuncPtr, origFunc>(origFunc));
	}

	template <typename OrigFuncPtr, OrigFuncPtr origFunc>
	void hookGdiTextDcFunction(const char* moduleName, const char* funcName)
	{
#ifdef DEBUGLOGS
		g_funcNames[origFunc] = funcName;
#endif

		Compat::hookFunction<OrigFuncPtr, origFunc>(
			moduleName, funcName, getCompatGdiTextDcFuncPtr<OrigFuncPtr, origFunc>(origFunc));
	}

	int WINAPI getRandomRgn(HDC hdc, HRGN hrgn, INT iNum)
	{
		int result = CALL_ORIG_FUNC(GetRandomRgn)(hdc, hrgn, iNum);
		if (1 != result)
		{
			return result;
		}

		HWND hwnd = WindowFromDC(hdc);
		if (!hwnd || hwnd == GetDesktopWindow() || (CALL_ORIG_FUNC(GetWindowLongA)(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED))
		{
			return 1;
		}

		ExcludeRgnForOverlappingWindowArgs args = { hrgn, GetAncestor(hwnd, GA_ROOT) };
		EnumWindows(excludeRgnForOverlappingWindow, reinterpret_cast<LPARAM>(&args));

		return 1;
	}

	template <typename WndClass, typename WndClassEx>
	ATOM WINAPI registerClass(const WndClass* lpWndClass, ATOM(WINAPI* origRegisterClass)(const WndClass*),
		ATOM(WINAPI* registerClassEx)(const WndClassEx*))
	{
		if (!lpWndClass)
		{
			return origRegisterClass(lpWndClass);
		}

		WndClassEx wc = {};
		wc.cbSize = sizeof(wc);
		memcpy(&wc.style, &lpWndClass->style, sizeof(WndClass));
		return registerClassEx(&wc);
	}

	template <typename WndClassEx>
	ATOM registerClassEx(const WndClassEx* lpWndClassEx,
		ATOM(WINAPI* origRegisterClassEx)(const WndClassEx*),
		decltype(&SetClassLong) origSetClassLong,
		decltype(&DefWindowProc) origDefWindowProc)
	{
		if (!lpWndClassEx || (!lpWndClassEx->hIcon && !lpWndClassEx->hIconSm))
		{
			return origRegisterClassEx(lpWndClassEx);
		}

		WndClassEx wc = *lpWndClassEx;
		wc.lpfnWndProc = origDefWindowProc;
		wc.hIcon = nullptr;
		wc.hIconSm = nullptr;

		ATOM atom = origRegisterClassEx(&wc);
		if (atom)
		{
			HWND hwnd = CreateWindow(reinterpret_cast<LPCSTR>(atom), "", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
			if (!hwnd)
			{
				UnregisterClass(reinterpret_cast<LPCSTR>(atom), GetModuleHandle(nullptr));
				return origRegisterClassEx(lpWndClassEx);
			}

			if (lpWndClassEx->hIcon)
			{
				SetClassLong(hwnd, GCL_HICON, reinterpret_cast<LONG>(lpWndClassEx->hIcon));
			}
			if (lpWndClassEx->hIconSm)
			{
				SetClassLong(hwnd, GCL_HICONSM, reinterpret_cast<LONG>(lpWndClassEx->hIconSm));
			}

			origSetClassLong(hwnd, GCL_WNDPROC, reinterpret_cast<LONG>(lpWndClassEx->lpfnWndProc));
			SetWindowLong(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(CALL_ORIG_FUNC(DefWindowProcA)));
			DestroyWindow(hwnd);
		}

		return atom;
	}

	ATOM WINAPI registerClassA(const WNDCLASSA* lpWndClass)
	{
		LOG_FUNC("RegisterClassA", lpWndClass);
		return LOG_RESULT(registerClass(lpWndClass, CALL_ORIG_FUNC(RegisterClassA), RegisterClassExA));
	}

	ATOM WINAPI registerClassW(const WNDCLASSW* lpWndClass)
	{
		LOG_FUNC("RegisterClassW", lpWndClass);
		return LOG_RESULT(registerClass(lpWndClass, CALL_ORIG_FUNC(RegisterClassW), RegisterClassExW));
	}

	ATOM WINAPI registerClassExA(const WNDCLASSEXA* lpWndClassEx)
	{
		LOG_FUNC("RegisterClassExA", lpWndClassEx);
		return LOG_RESULT(registerClassEx(lpWndClassEx, CALL_ORIG_FUNC(RegisterClassExA), CALL_ORIG_FUNC(SetClassLongA),
			CALL_ORIG_FUNC(DefWindowProcA)));
	}

	ATOM WINAPI registerClassExW(const WNDCLASSEXW* lpWndClassEx)
	{
		LOG_FUNC("RegisterClassExW", lpWndClassEx);
		return LOG_RESULT(registerClassEx(lpWndClassEx, CALL_ORIG_FUNC(RegisterClassExW), CALL_ORIG_FUNC(SetClassLongW),
			CALL_ORIG_FUNC(DefWindowProcW)));
	}

	DWORD WINAPI setClassLong(HWND hWnd, int nIndex, LONG dwNewLong, decltype(&SetClassLong) origSetClassLong)
	{
		if (GCL_HICON == nIndex || GCL_HICONSM == nIndex)
		{
			g_redirectToDib = false;
			DWORD result = origSetClassLong(hWnd, nIndex, dwNewLong);
			g_redirectToDib = true;
			return result;
		}
		return origSetClassLong(hWnd, nIndex, dwNewLong);
	}

	DWORD WINAPI setClassLongA(HWND hWnd, int nIndex, LONG dwNewLong)
	{
		LOG_FUNC("setClassLongA", hWnd, nIndex, dwNewLong);
		return LOG_RESULT(setClassLong(hWnd, nIndex, dwNewLong, CALL_ORIG_FUNC(SetClassLongA)));
	}

	DWORD WINAPI setClassLongW(HWND hWnd, int nIndex, LONG dwNewLong)
	{
		LOG_FUNC("setClassLongW", hWnd, nIndex, dwNewLong);
		return LOG_RESULT(setClassLong(hWnd, nIndex, dwNewLong, CALL_ORIG_FUNC(SetClassLongW)));
	}

	HWND WINAPI windowFromDc(HDC dc)
	{
		return CALL_ORIG_FUNC(WindowFromDC)(Gdi::Dc::getOrigDc(dc));
	}
}

#define HOOK_GDI_DC_FUNCTION(module, func) \
	hookGdiDcFunction<decltype(&func), &func>(#module, #func)

#define HOOK_GDI_TEXT_DC_FUNCTION(module, func) \
	hookGdiTextDcFunction<decltype(&func##A), &func##A>(#module, #func "A"); \
	hookGdiTextDcFunction<decltype(&func##W), &func##W>(#module, #func "W")

namespace Gdi
{
	namespace DcFunctions
	{
		HRGN getVisibleWindowRgn(HWND hwnd)
		{
			HRGN rgn = getWindowRegion(hwnd);
			ExcludeRgnForOverlappingWindowArgs args = { rgn, hwnd };
			EnumWindows(excludeRgnForOverlappingWindow, reinterpret_cast<LPARAM>(&args));
			return rgn;
		}

		void installHooks()
		{
			// Bitmap functions
			HOOK_GDI_DC_FUNCTION(msimg32, AlphaBlend);
			HOOK_GDI_DC_FUNCTION(gdi32, BitBlt);
			HOOK_FUNCTION(gdi32, CreateCompatibleBitmap, createCompatibleBitmap);
			HOOK_FUNCTION(gdi32, CreateDIBitmap, createDIBitmap);
			HOOK_FUNCTION(gdi32, CreateDiscardableBitmap, createDiscardableBitmap);
			HOOK_GDI_DC_FUNCTION(gdi32, ExtFloodFill);
			HOOK_GDI_DC_FUNCTION(gdi32, GdiAlphaBlend);
			HOOK_GDI_DC_FUNCTION(gdi32, GdiGradientFill);
			HOOK_GDI_DC_FUNCTION(gdi32, GdiTransparentBlt);
			HOOK_GDI_DC_FUNCTION(gdi32, GetDIBits);
			HOOK_GDI_DC_FUNCTION(gdi32, GetPixel);
			HOOK_GDI_DC_FUNCTION(msimg32, GradientFill);
			HOOK_GDI_DC_FUNCTION(gdi32, MaskBlt);
			HOOK_GDI_DC_FUNCTION(gdi32, PlgBlt);
			HOOK_GDI_DC_FUNCTION(gdi32, SetDIBits);
			HOOK_GDI_DC_FUNCTION(gdi32, SetDIBitsToDevice);
			HOOK_GDI_DC_FUNCTION(gdi32, SetPixel);
			HOOK_GDI_DC_FUNCTION(gdi32, SetPixelV);
			HOOK_GDI_DC_FUNCTION(gdi32, StretchBlt);
			HOOK_GDI_DC_FUNCTION(gdi32, StretchDIBits);
			HOOK_GDI_DC_FUNCTION(msimg32, TransparentBlt);

			// Brush functions
			HOOK_GDI_DC_FUNCTION(gdi32, PatBlt);

			// Clipping functions
			HOOK_FUNCTION(gdi32, GetRandomRgn, getRandomRgn);

			// Device context functions
			HOOK_GDI_DC_FUNCTION(gdi32, DrawEscape);
			HOOK_FUNCTION(user32, WindowFromDC, windowFromDc);

			// Filled shape functions
			HOOK_GDI_DC_FUNCTION(gdi32, Chord);
			HOOK_GDI_DC_FUNCTION(gdi32, Ellipse);
			HOOK_GDI_DC_FUNCTION(user32, FillRect);
			HOOK_GDI_DC_FUNCTION(user32, FrameRect);
			HOOK_GDI_DC_FUNCTION(user32, InvertRect);
			HOOK_GDI_DC_FUNCTION(gdi32, Pie);
			HOOK_GDI_DC_FUNCTION(gdi32, Polygon);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyPolygon);
			HOOK_GDI_DC_FUNCTION(gdi32, Rectangle);
			HOOK_GDI_DC_FUNCTION(gdi32, RoundRect);

			// Font and text functions
			HOOK_GDI_TEXT_DC_FUNCTION(user32, DrawText);
			HOOK_GDI_TEXT_DC_FUNCTION(user32, DrawTextEx);
			HOOK_GDI_TEXT_DC_FUNCTION(gdi32, ExtTextOut);
			HOOK_GDI_TEXT_DC_FUNCTION(gdi32, PolyTextOut);
			HOOK_GDI_TEXT_DC_FUNCTION(user32, TabbedTextOut);
			HOOK_GDI_TEXT_DC_FUNCTION(gdi32, TextOut);

			// Icon functions
			HOOK_GDI_DC_FUNCTION(user32, DrawIcon);
			HOOK_GDI_DC_FUNCTION(user32, DrawIconEx);

			// Line and curve functions
			HOOK_GDI_DC_FUNCTION(gdi32, AngleArc);
			HOOK_GDI_DC_FUNCTION(gdi32, Arc);
			HOOK_GDI_DC_FUNCTION(gdi32, ArcTo);
			HOOK_GDI_DC_FUNCTION(gdi32, LineTo);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyBezier);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyBezierTo);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyDraw);
			HOOK_GDI_DC_FUNCTION(gdi32, Polyline);
			HOOK_GDI_DC_FUNCTION(gdi32, PolylineTo);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyPolyline);

			// Painting and drawing functions
			HOOK_GDI_DC_FUNCTION(user32, DrawCaption);
			HOOK_GDI_DC_FUNCTION(user32, DrawEdge);
			HOOK_GDI_DC_FUNCTION(user32, DrawFocusRect);
			HOOK_GDI_DC_FUNCTION(user32, DrawFrameControl);
			HOOK_GDI_TEXT_DC_FUNCTION(user32, DrawState);
			HOOK_GDI_TEXT_DC_FUNCTION(user32, GrayString);
			HOOK_GDI_DC_FUNCTION(user32, PaintDesktop);

			// Region functions
			HOOK_GDI_DC_FUNCTION(gdi32, FillRgn);
			HOOK_GDI_DC_FUNCTION(gdi32, FrameRgn);
			HOOK_GDI_DC_FUNCTION(gdi32, InvertRgn);
			HOOK_GDI_DC_FUNCTION(gdi32, PaintRgn);

			// Scroll bar functions
			HOOK_GDI_DC_FUNCTION(user32, ScrollDC);

			// Undocumented functions
			HOOK_GDI_DC_FUNCTION(gdi32, GdiDrawStream);
			HOOK_GDI_DC_FUNCTION(gdi32, PolyPatBlt);

			// Window class functions
			HOOK_FUNCTION(user32, RegisterClassA, registerClassA);
			HOOK_FUNCTION(user32, RegisterClassW, registerClassW);
			HOOK_FUNCTION(user32, RegisterClassExA, registerClassExA);
			HOOK_FUNCTION(user32, RegisterClassExW, registerClassExW);
			HOOK_FUNCTION(user32, SetClassLongA, setClassLongA);
			HOOK_FUNCTION(user32, SetClassLongW, setClassLongW);
		}
	}
}
