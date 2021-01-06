#pragma once

#include <memory>
#include <vector>

#include <ddraw.h>

#include "Common/CompatPtr.h"
#include "Common/CompatRef.h"

namespace DDraw
{
	template <typename TSurface> class SurfaceImpl;
	template <typename TSurface> class SurfaceImpl2;

	class Surface
	{
	public:
		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*);
		virtual ULONG STDMETHODCALLTYPE AddRef();
		virtual ULONG STDMETHODCALLTYPE Release();

		Surface();
		virtual ~Surface();

		template <typename TDirectDraw, typename TSurface, typename TSurfaceDesc>
		static HRESULT create(
			CompatRef<TDirectDraw> dd, TSurfaceDesc desc, TSurface*& surface, std::unique_ptr<Surface> privateData);

		template <typename TSurface>
		static Surface* getSurface(TSurface& dds);

		template <typename TSurface>
		SurfaceImpl<TSurface>* getImpl() const;

		virtual void restore();

	protected:
		static void attach(CompatRef<IDirectDrawSurface7> dds, std::unique_ptr<Surface> privateData);

		virtual void createImpl();

		void* m_ddObject;
		std::unique_ptr<SurfaceImpl<IDirectDrawSurface>> m_impl;
		std::unique_ptr<SurfaceImpl<IDirectDrawSurface2>> m_impl2;
		std::unique_ptr<SurfaceImpl<IDirectDrawSurface3>> m_impl3;
		std::unique_ptr<SurfaceImpl<IDirectDrawSurface4>> m_impl4;
		std::unique_ptr<SurfaceImpl<IDirectDrawSurface7>> m_impl7;

		CompatWeakPtr<IDirectDrawSurface7> m_surface;

	private:
		template <typename TDirectDrawSurface>
		friend class SurfaceImpl;

		DWORD m_refCount;
	};
}
