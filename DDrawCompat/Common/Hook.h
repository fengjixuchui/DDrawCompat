#pragma once

#include <Windows.h>

#define CALL_ORIG_FUNC(func) Compat::getOrigFuncPtr<decltype(&func), &func>()

#define HOOK_FUNCTION(module, func, newFunc) \
	Compat::hookFunction<decltype(&func), &func>(#module, #func, &newFunc)
#define HOOK_SHIM_FUNCTION(func, newFunc) \
	Compat::hookFunction( \
		reinterpret_cast<void*&>(Compat::getOrigFuncPtr<decltype(&func), &func>()), newFunc, #func);


namespace Compat
{
	template <typename OrigFuncPtr, OrigFuncPtr origFunc>
	OrigFuncPtr& getOrigFuncPtr()
	{
		static OrigFuncPtr origFuncPtr = origFunc;
		return origFuncPtr;
	}

	FARPROC getProcAddress(HMODULE module, const char* procName);
	void hookFunction(void*& origFuncPtr, void* newFuncPtr, const char* funcName);
	void hookFunction(HMODULE module, const char* funcName, void*& origFuncPtr, void* newFuncPtr);
	void hookFunction(const char* moduleName, const char* funcName, void*& origFuncPtr, void* newFuncPtr);
	void hookIatFunction(HMODULE module, const char* importedModuleName, const char* funcName, void* newFuncPtr);

	template <typename OrigFuncPtr, OrigFuncPtr origFunc>
	void hookFunction(const char* moduleName, const char* funcName, OrigFuncPtr newFuncPtr)
	{
		hookFunction(moduleName, funcName,
			reinterpret_cast<void*&>(getOrigFuncPtr<OrigFuncPtr, origFunc>()), newFuncPtr);
	}

	void removeShim(HMODULE module, const char* funcName);
	void unhookAllFunctions();
	void unhookFunction(void* origFunc);
}
