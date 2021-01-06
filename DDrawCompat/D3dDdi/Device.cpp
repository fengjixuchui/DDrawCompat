#include <memory>
#include <sstream>

#include <d3d.h>
#include <winternl.h>
#include <../km/d3dkmthk.h>

#include <Common/HResultException.h>
#include <D3dDdi/Adapter.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/DeviceFuncs.h>
#include <D3dDdi/Resource.h>

namespace
{
	HANDLE g_gdiResourceHandle = nullptr;
	D3dDdi::Resource* g_gdiResource = nullptr;
	bool g_isReadOnlyGdiLockEnabled = false;

	void logSrcColorKeySupportFailure(const char* reason, UINT32 resultCode)
	{
		std::ostringstream oss;
		oss << "Checking source color key support: failed (" << reason;
		if (resultCode)
		{
			oss << ": " << Compat::hex(resultCode);
		}
		oss << ')';

		LOG_ONCE(oss.str().c_str());
	}
}

namespace D3dDdi
{
	Device::Device(HANDLE adapter, HANDLE device)
		: m_origVtable(*DeviceFuncs::s_origVtablePtr)
		, m_adapter(Adapter::get(adapter))
		, m_device(device)
		, m_isSrcColorKeySupported(checkSrcColorKeySupport())
		, m_renderTarget(nullptr)
		, m_renderTargetSubResourceIndex(0)
		, m_sharedPrimary(nullptr)
		, m_drawPrimitive(*this)
		, m_state(*this)
	{
	}

	HRESULT Device::blt(const D3DDDIARG_BLT* data)
	{
		flushPrimitives();
		auto it = m_resources.find(data->hDstResource);
		if (it != m_resources.end())
		{
			return it->second.blt(*data);
		}
		prepareForRendering(data->hSrcResource, data->SrcSubResourceIndex, true);
		return m_origVtable.pfnBlt(m_device, data);
	}

	bool Device::checkSrcColorKeySupport()
	{
		if (!(m_adapter.getDDrawCaps().CKeyCaps & DDRAW_CKEYCAPS_SRCBLT))
		{
			logSrcColorKeySupportFailure("driver indicates no support", 0);
			return false;
		}

		D3DDDI_SURFACEINFO si = {};
		si.Width = 2;
		si.Height = 1;

		D3DDDIARG_CREATERESOURCE2 cr = {};
		cr.Format = D3DDDIFMT_R5G6B5;
		cr.Pool = D3DDDIPOOL_VIDEOMEMORY;
		cr.pSurfList = &si;
		cr.SurfCount = 1;
		cr.Rotation = D3DDDI_ROTATION_IDENTITY;

		HRESULT result = createPrivateResource(cr);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("error creating source resource", result);
			return false;
		}
		auto resourceDeleter = [&](HANDLE resource) { m_origVtable.pfnDestroyResource(m_device, resource); };
		std::unique_ptr<void, std::function<void(HANDLE)>> srcRes(cr.hResource, resourceDeleter);

		cr.hResource = nullptr;
		cr.Flags.RenderTarget = 1;
		result = createPrivateResource(cr);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("error creating destination resource", result);
			return false;
		}
		std::unique_ptr<void, std::function<void(HANDLE)>> dstRes(cr.hResource, resourceDeleter);

		D3DDDIARG_LOCK lock = {};
		lock.hResource = srcRes.get();
		result = m_origVtable.pfnLock(m_device, &lock);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("error locking source resource", result);
			return false;
		}

		const UINT16 colorKey = 0xFA9F;
		*static_cast<UINT32*>(lock.pSurfData) = colorKey;

		D3DDDIARG_UNLOCK unlock = {};
		unlock.hResource = srcRes.get();
		m_origVtable.pfnUnlock(m_device, &unlock);

		lock = {};
		lock.hResource = dstRes.get();
		result = m_origVtable.pfnLock(m_device, &lock);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("error locking destination resource", result);
			return false;
		}

		*static_cast<UINT32*>(lock.pSurfData) = 0xFFFFFFFF;
		unlock.hResource = dstRes.get();
		m_origVtable.pfnUnlock(m_device, &unlock);

		D3DDDIARG_BLT blt = {};
		blt.hSrcResource = srcRes.get();
		blt.SrcRect = { 0, 0, 2, 1 };
		blt.hDstResource = dstRes.get();
		blt.DstRect = { 0, 0, 2, 1 };
		blt.ColorKey = colorKey;
		blt.Flags.SrcColorKey = 1;
		result = m_origVtable.pfnBlt(m_device, &blt);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("blt error", result);
			return false;
		}

		lock = {};
		lock.hResource = dstRes.get();
		result = m_origVtable.pfnLock(m_device, &lock);
		if (FAILED(result))
		{
			logSrcColorKeySupportFailure("error locking destination resource after blt", result);
			return false;
		}

		const UINT32 dstPixels = *static_cast<UINT32*>(lock.pSurfData);

		unlock.hResource = dstRes.get();
		m_origVtable.pfnUnlock(m_device, &unlock);

		if (dstPixels != 0xFFFF)
		{
			logSrcColorKeySupportFailure("test result pattern is incorrect", dstPixels);
			return false;
		}

		LOG_ONCE("Checking source color key support: passed");
		return true;
	}

	HRESULT Device::clear(const D3DDDIARG_CLEAR* data, UINT numRect, const RECT* rect)
	{
		flushPrimitives();
		if (data->Flags & D3DCLEAR_TARGET)
		{
			prepareForRendering();
		}
		return m_origVtable.pfnClear(m_device, data, numRect, rect);
	}

	HRESULT Device::colorFill(const D3DDDIARG_COLORFILL* data)
	{
		flushPrimitives();
		auto it = m_resources.find(data->hResource);
		if (it != m_resources.end())
		{
			return it->second.colorFill(*data);
		}
		return m_origVtable.pfnColorFill(m_device, data);
	}

	HRESULT Device::createPrivateResource(D3DDDIARG_CREATERESOURCE2& data)
	{
		if (m_origVtable.pfnCreateResource2)
		{
			return m_origVtable.pfnCreateResource2(m_device, &data);
		}
		return m_origVtable.pfnCreateResource(m_device, reinterpret_cast<D3DDDIARG_CREATERESOURCE*>(&data));
	}

	template <typename Arg>
	HRESULT Device::createResourceImpl(Arg& data)
	{
		try
		{
			Resource resource(*this, data);
			m_resources.emplace(resource, std::move(resource));
			if (data.Flags.VertexBuffer &&
				D3DDDIPOOL_SYSTEMMEM == data.Pool &&
				data.pSurfList[0].pSysMem)
			{
				m_drawPrimitive.addSysMemVertexBuffer(data.hResource,
					static_cast<BYTE*>(const_cast<void*>(data.pSurfList[0].pSysMem)), data.Fvf);
			}
			return S_OK;
		}
		catch (const HResultException& e)
		{
			return e.getResult();
		}
	}

	HRESULT Device::createResource(D3DDDIARG_CREATERESOURCE* data)
	{
		return createResourceImpl(*data);
	}

	HRESULT Device::createResource2(D3DDDIARG_CREATERESOURCE2* data)
	{
		return createResourceImpl(*data);
	}

	HRESULT Device::destroyResource(HANDLE resource)
	{
		flushPrimitives();
		if (g_gdiResource && resource == *g_gdiResource)
		{
			D3DDDIARG_LOCK lock = {};
			lock.hResource = *g_gdiResource;
			g_gdiResource->lock(lock);

			D3DDDIARG_UNLOCK unlock = {};
			unlock.hResource = *g_gdiResource;
			g_gdiResource->unlock(unlock);
		}

		if (resource == m_sharedPrimary)
		{
			D3DKMTReleaseProcessVidPnSourceOwners(GetCurrentProcess());
		}

		HRESULT result = m_origVtable.pfnDestroyResource(m_device, resource);
		if (SUCCEEDED(result))
		{
			m_resources.erase(resource);
			if (resource == m_sharedPrimary)
			{
				m_sharedPrimary = nullptr;
			}
			if (resource == g_gdiResourceHandle)
			{
				g_gdiResourceHandle = nullptr;
				g_gdiResource = nullptr;
			}
			m_drawPrimitive.removeSysMemVertexBuffer(resource);
		}

		return result;
	}

	HRESULT Device::drawIndexedPrimitive2(const D3DDDIARG_DRAWINDEXEDPRIMITIVE2* data,
		UINT /*indicesSize*/, const void* indexBuffer, const UINT* flagBuffer)
	{
		prepareForRendering();
		return m_drawPrimitive.drawIndexed(*data, static_cast<const UINT16*>(indexBuffer), flagBuffer);
	}

	HRESULT Device::drawPrimitive(const D3DDDIARG_DRAWPRIMITIVE* data, const UINT* flagBuffer)
	{
		prepareForRendering();
		return m_drawPrimitive.draw(*data, flagBuffer);
	}

	HRESULT Device::flush()
	{
		if (!s_isFlushEnabled)
		{
			return S_OK;
		}
		flushPrimitives();
		return m_origVtable.pfnFlush(m_device);
	}

	HRESULT Device::flush1(UINT FlushFlags)
	{
		if (!s_isFlushEnabled && 0 == FlushFlags)
		{
			return S_OK;
		}
		flushPrimitives();
		return m_origVtable.pfnFlush1(m_device, FlushFlags);
	}

	HRESULT Device::lock(D3DDDIARG_LOCK* data)
	{
		flushPrimitives();
		auto it = m_resources.find(data->hResource);
		if (it != m_resources.end())
		{
			return it->second.lock(*data);
		}
		return m_origVtable.pfnLock(m_device, data);
	}

	HRESULT Device::openResource(D3DDDIARG_OPENRESOURCE* data)
	{
		HRESULT result = m_origVtable.pfnOpenResource(m_device, data);
		if (SUCCEEDED(result) && data->Flags.Fullscreen)
		{
			m_sharedPrimary = data->hResource;
		}
		return result;
	}

	HRESULT Device::present(const D3DDDIARG_PRESENT* data)
	{
		flushPrimitives();
		prepareForRendering(data->hSrcResource, data->SrcSubResourceIndex, true);
		return m_origVtable.pfnPresent(m_device, data);
	}

	HRESULT Device::present1(D3DDDIARG_PRESENT1* data)
	{
		flushPrimitives();
		for (UINT i = 0; i < data->SrcResources; ++i)
		{
			prepareForRendering(data->phSrcResources[i].hResource, data->phSrcResources[i].SubResourceIndex, true);
		}
		return m_origVtable.pfnPresent1(m_device, data);
	}

	HRESULT Device::setRenderTarget(const D3DDDIARG_SETRENDERTARGET* data)
	{
		flushPrimitives();
		HRESULT result = m_origVtable.pfnSetRenderTarget(m_device, data);
		if (SUCCEEDED(result) && 0 == data->RenderTargetIndex)
		{
			m_renderTarget = getResource(data->hRenderTarget);
			m_renderTargetSubResourceIndex = data->SubResourceIndex;
		}
		return result;
	}

	HRESULT Device::setStreamSource(const D3DDDIARG_SETSTREAMSOURCE* data)
	{
		return m_drawPrimitive.setStreamSource(*data);
	}

	HRESULT Device::setStreamSourceUm(const D3DDDIARG_SETSTREAMSOURCEUM* data, const void* umBuffer)
	{
		return m_drawPrimitive.setStreamSourceUm(*data, umBuffer);
	}

	HRESULT Device::unlock(const D3DDDIARG_UNLOCK* data)
	{
		flushPrimitives();
		auto it = m_resources.find(data->hResource);
		if (it != m_resources.end())
		{
			return it->second.unlock(*data);
		}
		return m_origVtable.pfnUnlock(m_device, data);
	}

	Resource* Device::getGdiResource()
	{
		return g_gdiResource;
	}

	void Device::prepareForRendering(HANDLE resource, UINT subResourceIndex, bool isReadOnly)
	{
		auto it = m_resources.find(resource);
		if (it != m_resources.end())
		{
			it->second.prepareForRendering(subResourceIndex, isReadOnly);
		}
	}

	void Device::prepareForRendering()
	{
		if (m_renderTarget)
		{
			m_renderTarget->prepareForRendering(m_renderTargetSubResourceIndex, false);
		}
	}

	void Device::add(HANDLE adapter, HANDLE device)
	{
		s_devices.try_emplace(device, adapter, device);
	}

	Device& Device::get(HANDLE device)
	{
		auto it = s_devices.find(device);
		if (it != s_devices.end())
		{
			return it->second;
		}

		return s_devices.try_emplace(device, nullptr, device).first->second;
	}

	void Device::remove(HANDLE device)
	{
		s_devices.erase(device);
	}

	Resource* Device::getResource(HANDLE resource)
	{
		auto it = m_resources.find(resource);
		return it != m_resources.end() ? &it->second : nullptr;
	}

	Resource* Device::findResource(HANDLE resource)
	{
		for (auto& device : s_devices)
		{
			auto res = device.second.getResource(resource);
			if (res)
			{
				return res;
			}
		}
		return nullptr;
	}

	void Device::setGdiResourceHandle(HANDLE resource)
	{
		ScopedCriticalSection lock;
		if ((!resource && !g_gdiResource) ||
			(g_gdiResource && resource == *g_gdiResource))
		{
			return;
		}

		if (g_gdiResource)
		{
			g_gdiResource->setAsGdiResource(false);
		}

		g_gdiResourceHandle = resource;
		g_gdiResource = findResource(resource);

		if (g_gdiResource)
		{
			g_gdiResource->setAsGdiResource(true);
		}
	}

	void Device::setReadOnlyGdiLock(bool enable)
	{
		g_isReadOnlyGdiLockEnabled = enable;
	}

	std::map<HANDLE, Device> Device::s_devices;
	bool Device::s_isFlushEnabled = true;
}
