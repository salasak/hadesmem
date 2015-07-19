// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#include "d3d9.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>
#include <winnt.h>
#include <winternl.h>

#include <d3d9.h>

#include <hadesmem/config.hpp>
#include <hadesmem/detail/last_error_preserver.hpp>
#include <hadesmem/detail/winternl.hpp>
#include <hadesmem/find_procedure.hpp>
#include <hadesmem/patcher.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/process_helpers.hpp>
#include <hadesmem/region.hpp>

#include "callbacks.hpp"
#include "helpers.hpp"
#include "hook_counter.hpp"
#include "hook_disabler.hpp"
#include "main.hpp"
#include "module.hpp"
#include "process.hpp"

// TODO: Clean up code duplication caused by adding device to the map in all
// funcs. (Use a helper func instead.)

// TODO: Implement the AddRef/Release support.

// TODO: Fix up the ref counting, reentrancy issues, etc. Everything is a major
// hack right now...

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace
{
class D3D9Impl : public hadesmem::cerberus::D3D9Interface
{
public:
  virtual std::size_t RegisterOnFrame(
    std::function<hadesmem::cerberus::OnFrameD3D9Callback> const& callback)
    final
  {
    auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
    return callbacks.Register(callback);
  }

  virtual void UnregisterOnFrame(std::size_t id) final
  {
    auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
    return callbacks.Unregister(id);
  }

  virtual std::size_t RegisterOnReset(
    std::function<hadesmem::cerberus::OnResetD3D9Callback> const& callback)
    final
  {
    auto& callbacks = hadesmem::cerberus::GetOnResetD3D9Callbacks();
    return callbacks.Register(callback);
  }

  virtual void UnregisterOnReset(std::size_t id) final
  {
    auto& callbacks = hadesmem::cerberus::GetOnResetD3D9Callbacks();
    return callbacks.Unregister(id);
  }

  virtual std::size_t RegisterOnRelease(
    std::function<hadesmem::cerberus::OnReleaseD3D9Callback> const& callback)
    final
  {
    auto& callbacks = hadesmem::cerberus::GetOnReleaseD3D9Callbacks();
    return callbacks.Register(callback);
  }

  virtual void UnregisterOnRelease(std::size_t id) final
  {
    auto& callbacks = hadesmem::cerberus::GetOnReleaseD3D9Callbacks();
    return callbacks.Unregister(id);
  }
};

std::uint32_t& GetReleaseHookCount() HADESMEM_DETAIL_NOEXCEPT
{
  static __declspec(thread) std::uint32_t in_hook = 0;
  return in_hook;
}

std::uint32_t& GetPresentHookCount() HADESMEM_DETAIL_NOEXCEPT
{
  static __declspec(thread) std::uint32_t in_hook = 0;
  return in_hook;
}

std::uint32_t& GetResetHookCount() HADESMEM_DETAIL_NOEXCEPT
{
  static __declspec(thread) std::uint32_t in_hook = 0;
  return in_hook;
}

struct DeviceData
{
  std::uint64_t ref_count_;
};

std::map<IDirect3DDevice9*, DeviceData>& GetDeviceMap()
{
  static std::map<IDirect3DDevice9*, DeviceData> device_map;
  return device_map;
}

std::mutex& GetDeviceMapMutex()
{
  static std::mutex mutex;
  return mutex;
}

typedef ULONG(WINAPI* IDirect3DDevice9_AddRef_Fn)(IDirect3DDevice9* device);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_AddRef_Fn>>&
  GetIDirect3DDevice9AddRefDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_AddRef_Fn>>
    detour;
  return detour;
}

extern "C" ULONG WINAPI
  IDirect3DDevice9_AddRef_Detour(hadesmem::PatchDetourBase* detour,
                                 IDirect3DDevice9* device)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetReleaseHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p].", device);

  auto const add_ref = detour->GetTrampolineT<IDirect3DDevice9_AddRef_Fn>();
  last_error_preserver.Revert();
  auto ret = add_ref(device);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%lu].", ret);

  return ret;
}

typedef ULONG(WINAPI* IDirect3DDevice9_Release_Fn)(IDirect3DDevice9* device);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Release_Fn>>&
  GetIDirect3DDevice9ReleaseDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Release_Fn>>
    detour;
  return detour;
}

extern "C" ULONG WINAPI
  IDirect3DDevice9_Release_Detour(hadesmem::PatchDetourBase* detour,
                                  IDirect3DDevice9* device)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetReleaseHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p].", device);

  auto const release = detour->GetTrampolineT<IDirect3DDevice9_Release_Fn>();
  last_error_preserver.Revert();
  auto ret = release(device);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%lu].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DDevice9_EndScene_Fn)(IDirect3DDevice9* device);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_EndScene_Fn>>&
  GetIDirect3DDevice9EndSceneDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_EndScene_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DDevice9_EndScene_Detour(hadesmem::PatchDetourBase* detour,
                                   IDirect3DDevice9* device)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetPresentHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p].", device);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    auto& mutex = GetDeviceMapMutex();
    std::lock_guard<std::mutex> lock{mutex};

    auto& device_map = GetDeviceMap();
    auto const iter = device_map.find(device);
    if (iter == std::end(device_map))
    {
      DeviceData data = {};
      data.ref_count_ = 1;
      device_map[device] = data;
    }

    auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
    callbacks.Run(device);
  }

  auto const end_scene = detour->GetTrampolineT<IDirect3DDevice9_EndScene_Fn>();
  last_error_preserver.Revert();
  auto ret = end_scene(device);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DDevice9_Present_Fn)(
  IDirect3DDevice9* device,
  const RECT* pSourceRect,
  const RECT* pDestRect,
  HWND hDestWindowOverride,
  const RGNDATA* pDirtyRegion);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Present_Fn>>&
  GetIDirect3DDevice9PresentDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Present_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DDevice9_Present_Detour(hadesmem::PatchDetourBase* detour,
                                  IDirect3DDevice9* device,
                                  const RECT* source_rect,
                                  const RECT* dest_rect,
                                  HWND dest_window_override,
                                  const RGNDATA* dirty_region)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetPresentHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p] [%p] [%p] [%p] [%p].",
                                       device,
                                       source_rect,
                                       dest_rect,
                                       dest_window_override,
                                       dirty_region);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    auto& mutex = GetDeviceMapMutex();
    std::lock_guard<std::mutex> lock{mutex};

    auto& device_map = GetDeviceMap();
    auto const iter = device_map.find(device);
    if (iter == std::end(device_map))
    {
      DeviceData data = {};
      data.ref_count_ = 1;
      device_map[device] = data;
    }

    auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
    callbacks.Run(device);
  }

  auto const present = detour->GetTrampolineT<IDirect3DDevice9_Present_Fn>();
  last_error_preserver.Revert();
  auto ret =
    present(device, source_rect, dest_rect, dest_window_override, dirty_region);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DDevice9Ex_PresentEx_Fn)(
  IDirect3DDevice9Ex* device,
  const RECT* pSourceRect,
  const RECT* pDestRect,
  HWND hDestWindowOverride,
  const RGNDATA* pDirtyRegion,
  DWORD dwFlags);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9Ex_PresentEx_Fn>>&
  GetIDirect3DDevice9ExPresentExDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9Ex_PresentEx_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DDevice9Ex_PresentEx_Detour(hadesmem::PatchDetourBase* detour,
                                      IDirect3DDevice9Ex* device,
                                      const RECT* source_rect,
                                      const RECT* dest_rect,
                                      HWND dest_window_override,
                                      const RGNDATA* dirty_region,
                                      DWORD flags)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetPresentHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p] [%p] [%p] [%p] [%p] [%lu].",
                                       device,
                                       source_rect,
                                       dest_rect,
                                       dest_window_override,
                                       dirty_region,
                                       flags);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    auto& mutex = GetDeviceMapMutex();
    std::lock_guard<std::mutex> lock{mutex};

    auto& device_map = GetDeviceMap();
    auto const iter = device_map.find(device);
    if (iter == std::end(device_map))
    {
      DeviceData data = {};
      data.ref_count_ = 1;
      device_map[device] = data;
    }

    auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
    callbacks.Run(device);
  }

  auto const present =
    detour->GetTrampolineT<IDirect3DDevice9Ex_PresentEx_Fn>();
  last_error_preserver.Revert();
  auto ret = present(
    device, source_rect, dest_rect, dest_window_override, dirty_region, flags);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DSwapChain9_Present_Fn)(
  IDirect3DSwapChain9* swap_chain,
  const RECT* pSourceRect,
  const RECT* pDestRect,
  HWND hDestWindowOverride,
  const RGNDATA* pDirtyRegion,
  DWORD dwFlags);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DSwapChain9_Present_Fn>>&
  GetIDirect3DSwapChain9PresentDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DSwapChain9_Present_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DSwapChain9_Present_Detour(hadesmem::PatchDetourBase* detour,
                                     IDirect3DSwapChain9* swap_chain,
                                     const RECT* source_rect,
                                     const RECT* dest_rect,
                                     HWND dest_window_override,
                                     const RGNDATA* dirty_region,
                                     DWORD flags)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetPresentHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p] [%p] [%p] [%p] [%p] [%lu].",
                                       swap_chain,
                                       source_rect,
                                       dest_rect,
                                       dest_window_override,
                                       dirty_region,
                                       flags);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    IDirect3DDevice9* device = nullptr;
    auto const get_device_hr = swap_chain->GetDevice(&device);
    if (SUCCEEDED(get_device_hr))
    {
      auto& mutex = GetDeviceMapMutex();
      std::lock_guard<std::mutex> lock{mutex};

      auto& device_map = GetDeviceMap();
      auto const iter = device_map.find(device);
      if (iter == std::end(device_map))
      {
        DeviceData data = {};
        data.ref_count_ = 1;
        device_map[device] = data;
      }

      hadesmem::detail::SmartComHandle smart_device{device};
      auto& callbacks = hadesmem::cerberus::GetOnFrameD3D9Callbacks();
      callbacks.Run(device);
    }
    else
    {
      HADESMEM_DETAIL_TRACE_FORMAT_A(
        "WARNING! IDirect3DSwapChain9::GetDevice failed. HR: [%lX].",
        get_device_hr);
    }
  }

  auto const present = detour->GetTrampolineT<IDirect3DSwapChain9_Present_Fn>();
  last_error_preserver.Revert();
  auto ret = present(swap_chain,
                     source_rect,
                     dest_rect,
                     dest_window_override,
                     dirty_region,
                     flags);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DDevice9_Reset_Fn)(
  IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Reset_Fn>>&
  GetIDirect3DDevice9ResetDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9_Reset_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DDevice9_Reset_Detour(hadesmem::PatchDetourBase* detour,
                                IDirect3DDevice9* device,
                                D3DPRESENT_PARAMETERS* presentation_params)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetResetHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A(
    "Args: [%p] [%p].", device, presentation_params);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    auto& mutex = GetDeviceMapMutex();
    std::lock_guard<std::mutex> lock{mutex};

    auto& device_map = GetDeviceMap();
    auto const iter = device_map.find(device);
    if (iter == std::end(device_map))
    {
      DeviceData data = {};
      data.ref_count_ = 1;
      device_map[device] = data;
    }

    auto& callbacks = hadesmem::cerberus::GetOnResetD3D9Callbacks();
    callbacks.Run(device, presentation_params);
  }

  auto const reset = detour->GetTrampolineT<IDirect3DDevice9_Reset_Fn>();
  last_error_preserver.Revert();
  auto ret = reset(device, presentation_params);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

typedef HRESULT(WINAPI* IDirect3DDevice9Ex_ResetEx_Fn)(
  IDirect3DDevice9Ex* device,
  D3DPRESENT_PARAMETERS* pPresentationParameters,
  D3DDISPLAYMODEEX* pFullscreenDisplayMode);

std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9Ex_ResetEx_Fn>>&
  GetIDirect3DDevice9ExResetExDetour() HADESMEM_DETAIL_NOEXCEPT
{
  static std::unique_ptr<hadesmem::PatchDetour<IDirect3DDevice9Ex_ResetEx_Fn>>
    detour;
  return detour;
}

extern "C" HRESULT WINAPI
  IDirect3DDevice9Ex_ResetEx_Detour(hadesmem::PatchDetourBase* detour,
                                    IDirect3DDevice9Ex* device,
                                    D3DPRESENT_PARAMETERS* presentation_params,
                                    D3DDISPLAYMODEEX* fullscreen_display_mode)
{
  hadesmem::detail::LastErrorPreserver last_error_preserver;
  hadesmem::cerberus::HookCounter hook_counter{&GetResetHookCount()};

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Args: [%p] [%p] [%p].",
                                       device,
                                       presentation_params,
                                       fullscreen_display_mode);

  auto const hook_count = hook_counter.GetCount();
  HADESMEM_DETAIL_ASSERT(hook_count > 0);
  if (hook_count == 1)
  {
    auto& mutex = GetDeviceMapMutex();
    std::lock_guard<std::mutex> lock{mutex};

    auto& device_map = GetDeviceMap();
    auto const iter = device_map.find(device);
    if (iter == std::end(device_map))
    {
      DeviceData data = {};
      data.ref_count_ = 1;
      device_map[device] = data;
    }

    auto& callbacks = hadesmem::cerberus::GetOnResetD3D9Callbacks();
    callbacks.Run(device, presentation_params);
  }

  auto const reset_ex = detour->GetTrampolineT<IDirect3DDevice9Ex_ResetEx_Fn>();
  last_error_preserver.Revert();
  auto ret = reset_ex(device, presentation_params, fullscreen_display_mode);
  last_error_preserver.Update();

  HADESMEM_DETAIL_TRACE_NOISY_FORMAT_A("Ret: [%ld].", ret);

  return ret;
}

std::pair<void*, SIZE_T>& GetD3D9Module() HADESMEM_DETAIL_NOEXCEPT
{
  static std::pair<void*, SIZE_T> module{};
  return module;
}

LRESULT
CALLBACK
WrapDefWindowProcW(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}

namespace hadesmem
{
namespace cerberus
{
Callbacks<OnFrameD3D9Callback>& GetOnFrameD3D9Callbacks()
{
  static Callbacks<OnFrameD3D9Callback> callbacks;
  return callbacks;
}

Callbacks<OnResetD3D9Callback>& GetOnResetD3D9Callbacks()
{
  static Callbacks<OnResetD3D9Callback> callbacks;
  return callbacks;
}

Callbacks<OnReleaseD3D9Callback>& GetOnReleaseD3D9Callbacks()
{
  static Callbacks<OnReleaseD3D9Callback> callbacks;
  return callbacks;
}

D3D9Interface& GetD3D9Interface() HADESMEM_DETAIL_NOEXCEPT
{
  static D3D9Impl d3d9_impl;
  return d3d9_impl;
}

void InitializeD3D9()
{
  auto& helper = GetHelperInterface();
  helper.InitializeSupportForModule(
    L"D3D9", DetourD3D9, UndetourD3D9, GetD3D9Module, false);
}

void DetourD3D9(HMODULE base)
{
  auto const& process = GetThisProcess();
  auto& module = GetD3D9Module();
  auto& helper = GetHelperInterface();
  if (helper.CommonDetourModule(process, L"D3D9", base, module))
  {
    auto const pid_str = std::to_wstring(::GetCurrentProcessId());

    auto const file_mapping_name = CERBERUS_HELPER_D3D9_MAP_NAME + pid_str;
    HADESMEM_DETAIL_TRACE_FORMAT_W(L"Helper mapping name: [%s].",
                                   file_mapping_name.c_str());
    hadesmem::detail::SmartHandle file_mapping{
      ::CreateFileMappingW(INVALID_HANDLE_VALUE,
                           nullptr,
                           PAGE_READWRITE,
                           0,
                           sizeof(hadesmem::cerberus::D3D9Offsets),
                           file_mapping_name.c_str())};
    if (!file_mapping.IsValid())
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"CreateFileMappingW failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }

    hadesmem::detail::SmartMappedFileHandle mapping_view{::MapViewOfFileEx(
      file_mapping.GetHandle(), FILE_MAP_READ, 0, 0, 0, nullptr)};
    if (!mapping_view.IsValid())
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"MapViewOfFileEx failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }

    hadesmem::cerberus::HookDisabler disable_create_process_hook{
      &hadesmem::cerberus::GetDisableCreateProcessInternalWHook()};

    auto const self_dir_path = hadesmem::detail::GetSelfDirPath();
    std::wstring const helper_path =
      hadesmem::detail::CombinePath(self_dir_path, L"cerberus_helper.exe");
    auto const helper_command_line = L"\"" + helper_path + L"\" " + pid_str;
    std::vector<wchar_t> command_line_buf(std::begin(helper_command_line),
                                          std::end(helper_command_line));
    command_line_buf.push_back(L'\0');

    STARTUPINFO start_info{};
    PROCESS_INFORMATION proc_info{};
    if (!::CreateProcessW(nullptr,
                          command_line_buf.data(),
                          nullptr,
                          nullptr,
                          FALSE,
                          0,
                          nullptr,
                          nullptr,
                          &start_info,
                          &proc_info))
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"CreateProcessW failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }

    hadesmem::detail::SmartHandle const helper_process_handle{
      proc_info.hProcess};
    hadesmem::detail::SmartHandle const helper_thread_handle{proc_info.hThread};

    DWORD const wait_res =
      ::WaitForSingleObject(helper_process_handle.GetHandle(), INFINITE);
    if (wait_res != WAIT_OBJECT_0)
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{}
        << hadesmem::ErrorString{"WaitForSingleObject failed."}
        << hadesmem::ErrorCodeWinLast{last_error});
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeProcess(helper_process_handle.GetHandle(), &exit_code))
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"GetExitCodeProcess failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }

    if (exit_code != 0)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"Helper failed."});
    }

    auto const d3d9_offsets =
      static_cast<D3D9Offsets*>(mapping_view.GetHandle());
    auto const offset_base = reinterpret_cast<std::uint8_t*>(base);

#if 0
    auto const add_ref_fn = offset_base + d3d9_offsets->add_ref_;
    DetourFunc(process,
               "IDirect3DDevice9::AddRef",
               GetIDirect3DDevice9AddRefDetour(),
               reinterpret_cast<IDirect3DDevice9_AddRef_Fn>(add_ref_fn),
               IDirect3DDevice9_AddRef_Detour);

    auto const release_fn = offset_base + d3d9_offsets->release_;
    DetourFunc(process,
               "IDirect3DDevice9::Release",
               GetIDirect3DDevice9ReleaseDetour(),
               reinterpret_cast<IDirect3DDevice9_Release_Fn>(release_fn),
               IDirect3DDevice9_Release_Detour);
#endif

    auto const present_fn = offset_base + d3d9_offsets->present_;
    DetourFunc(process,
               "IDirect3DDevice9::Present",
               GetIDirect3DDevice9PresentDetour(),
               reinterpret_cast<IDirect3DDevice9_Present_Fn>(present_fn),
               IDirect3DDevice9_Present_Detour);

    auto const reset_fn = offset_base + d3d9_offsets->reset_;
    DetourFunc(process,
               "IDirect3DDevice9::Reset",
               GetIDirect3DDevice9ResetDetour(),
               reinterpret_cast<IDirect3DDevice9_Reset_Fn>(reset_fn),
               IDirect3DDevice9_Reset_Detour);

    auto const end_scene_fn = offset_base + d3d9_offsets->end_scene_;
    DetourFunc(process,
               "IDirect3DDevice9::EndScene",
               GetIDirect3DDevice9EndSceneDetour(),
               reinterpret_cast<IDirect3DDevice9_EndScene_Fn>(end_scene_fn),
               IDirect3DDevice9_EndScene_Detour);

    auto const present_ex_fn = offset_base + d3d9_offsets->present_ex_;
    DetourFunc(process,
               "IDirect3DDevice9Ex::PresentEx",
               GetIDirect3DDevice9ExPresentExDetour(),
               reinterpret_cast<IDirect3DDevice9Ex_PresentEx_Fn>(present_ex_fn),
               IDirect3DDevice9Ex_PresentEx_Detour);

    auto const reset_ex_fn = offset_base + d3d9_offsets->reset_ex_;
    DetourFunc(process,
               "IDirect3DDevice9Ex::ResetEx",
               GetIDirect3DDevice9ExResetExDetour(),
               reinterpret_cast<IDirect3DDevice9Ex_ResetEx_Fn>(reset_ex_fn),
               IDirect3DDevice9Ex_ResetEx_Detour);

    auto const swap_chain_present_fn =
      offset_base + d3d9_offsets->swap_chain_present_;
    DetourFunc(
      process,
      "IDirect3DSwapChain9::Present",
      GetIDirect3DSwapChain9PresentDetour(),
      reinterpret_cast<IDirect3DSwapChain9_Present_Fn>(swap_chain_present_fn),
      IDirect3DSwapChain9_Present_Detour);
  }
}

void UndetourD3D9(bool remove)
{
  auto& module = GetD3D9Module();
  auto& helper = GetHelperInterface();
  if (helper.CommonUndetourModule(L"D3D9", module))
  {
    UndetourFunc(
      L"IDirect3DDevice9::AddRef", GetIDirect3DDevice9AddRefDetour(), remove);
    UndetourFunc(
      L"IDirect3DDevice9::Release", GetIDirect3DDevice9ReleaseDetour(), remove);
    UndetourFunc(
      L"IDirect3DDevice9::Present", GetIDirect3DDevice9PresentDetour(), remove);
    UndetourFunc(
      L"IDirect3DDevice9::Reset", GetIDirect3DDevice9ResetDetour(), remove);
    UndetourFunc(L"IDirect3DDevice9::EndScene",
                 GetIDirect3DDevice9EndSceneDetour(),
                 remove);
    UndetourFunc(L"IDirect3DDevice9Ex::PresentEx",
                 GetIDirect3DDevice9ExPresentExDetour(),
                 remove);
    UndetourFunc(L"IDirect3DDevice9Ex::ResetEx",
                 GetIDirect3DDevice9ExResetExDetour(),
                 remove);
    UndetourFunc(L"IDirect3DSwapChain9::Present",
                 GetIDirect3DSwapChain9PresentDetour(),
                 remove);

    module = std::make_pair(nullptr, 0);
  }
}
}
}
