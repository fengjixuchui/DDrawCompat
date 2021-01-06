#include <map>

#include "D3dDdi/Adapter.h"
#include "D3dDdi/AdapterFuncs.h"
#include "D3dDdi/DeviceCallbacks.h"
#include "D3dDdi/DeviceFuncs.h"

namespace
{
	HRESULT APIENTRY closeAdapter(HANDLE hAdapter)
	{
		HRESULT result = D3dDdi::AdapterFuncs::s_origVtablePtr->pfnCloseAdapter(hAdapter);
		if (SUCCEEDED(result))
		{
			D3dDdi::Adapter::remove(hAdapter);
		}
		return result;
	}

	HRESULT APIENTRY createDevice(HANDLE hAdapter, D3DDDIARG_CREATEDEVICE* pCreateData)
	{
		D3dDdi::DeviceCallbacks::hookVtable(pCreateData->pCallbacks);
		HRESULT result = D3dDdi::AdapterFuncs::s_origVtablePtr->pfnCreateDevice(hAdapter, pCreateData);
		if (SUCCEEDED(result))
		{
			D3dDdi::DeviceFuncs::hookVtable(
				D3dDdi::Adapter::get(hAdapter).getModule(), pCreateData->pDeviceFuncs);
			D3dDdi::DeviceFuncs::onCreateDevice(hAdapter, pCreateData->hDevice);
		}
		return result;
	}

	HRESULT APIENTRY getCaps(HANDLE hAdapter, const D3DDDIARG_GETCAPS* pData)
	{
		HRESULT result = D3dDdi::AdapterFuncs::s_origVtablePtr->pfnGetCaps(hAdapter, pData);
		if (SUCCEEDED(result) && D3DDDICAPS_DDRAW == pData->Type)
		{
			static_cast<DDRAW_CAPS*>(pData->pData)->FxCaps =
				DDRAW_FXCAPS_BLTMIRRORLEFTRIGHT | DDRAW_FXCAPS_BLTMIRRORUPDOWN;
		}
		return result;
	}
}

namespace D3dDdi
{
	void AdapterFuncs::onOpenAdapter(HANDLE adapter, HMODULE module)
	{
		Adapter::add(adapter, module);
	}

	void AdapterFuncs::setCompatVtable(D3DDDI_ADAPTERFUNCS& vtable)
	{
		vtable.pfnCloseAdapter = &closeAdapter;
		vtable.pfnCreateDevice = &createDevice;
		vtable.pfnGetCaps = &getCaps;
	}
}
