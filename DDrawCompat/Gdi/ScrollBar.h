#pragma once

#include <Windows.h>

namespace Gdi
{
	class ScrollBar
	{
	public:
		ScrollBar(HWND hwnd, HDC compatDc);

		void drawAll() const;
		void drawHorizArrows() const;
		void drawVertArrows() const;
		void excludeFromClipRegion() const;

	private:
		struct ScrollBarChildInfo
		{
			RECT rect;
			LONG state;
		};

		struct ScrollBarInfo
		{
			ScrollBarChildInfo topLeftArrow;
			ScrollBarChildInfo bottomRightArrow;
			bool isVisible;
		};

		void drawArrow(const ScrollBarChildInfo& sbci, UINT dfcState) const;
		void excludeFromClipRegion(const RECT& rect) const;
		void excludeFromClipRegion(const ScrollBarInfo& sbi) const;
		ScrollBarInfo getScrollBarInfo(LONG objId) const;
		void setPressedState(ScrollBarChildInfo& sbci) const;

		HWND m_hwnd;
		HDC m_compatDc;
		RECT m_windowRect;
		bool m_isLeftMouseButtonDown;
		POINT m_cursorPos;
		ScrollBarInfo m_horizontalSbi;
		ScrollBarInfo m_verticalSbi;
	};
}
