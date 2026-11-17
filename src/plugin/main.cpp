// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program (LICENSE.txt).
// If not, see <https://www.gnu.org/licenses/>.

#include "Audio.h"
#include "Config.h"
#include "Menu.h"
#include "Overlay.h"
#include "Performance.h"

// One DLL for every version: CommonLibSSE-NG built for SE and AE, every game address taken from Address Library by
// ID, and the plugin declared version-independent (add_commonlibsse_plugin ... USE_ADDRESS_LIBRARY), so it loads on
// 1.5.97, every 1.6 release (1.6.1170 included) and later ones Address Library covers, Steam and GOG.
namespace
{
	void SetupLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "SkyrimSongs.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void LogEnvironment()
	{
		const auto ver = REL::Module::get().version();
		spdlog::info("Game version {} ({})", ver.string("."sv), REL::Module::IsAE() ? (ver[1] >= 7 ? "AE 1.7" : "AE") : (REL::Module::IsSE() ? "SE" : "other"));
		const auto lib = REL::Module::IsAE() ? fmt::format("Data\\SKSE\\Plugins\\versionlib-{}.bin", ver.string()) :
		                                       fmt::format("Data\\SKSE\\Plugins\\version-{}.bin", ver.string());
		std::error_code ec;
		if (std::filesystem::exists(lib, ec)) {
			spdlog::info("Address Library found: {}", lib);
		} else {
			spdlog::critical("Address Library NOT FOUND: {} - install 'Address Library for SKSE Plugins' for this game version", lib);
		}
	}

	// ------------------------------------------------------------------ Present hook (as in Hearthsworn and Yacht)
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFn         g_originalPresent = nullptr;
	std::atomic<bool> g_presentDisabled{ false };
	std::atomic<bool> g_tickDisabled{ false };
	std::atomic<bool> g_tickQueued{ false };

	void SafeTick()
	{
		__try {
			Performance::FrameTick();
		} __except (Overlay::SehFilter(GetExceptionInformation(), "FrameTick")) {
			g_tickDisabled = true;
		}
	}

	void SafeOnPresent(IDXGISwapChain* a_sc)
	{
		__try {
			Overlay::OnPresent(a_sc);
		} __except (Overlay::SehFilter(GetExceptionInformation(), "OnPresent")) {
			g_presentDisabled = true;
		}
	}

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_sc, UINT a_sync, UINT a_flags)
	{
		if (!g_tickDisabled && !g_tickQueued.exchange(true)) {
			SKSE::GetTaskInterface()->AddTask([] {
				g_tickQueued = false;
				if (!g_tickDisabled) {
					SafeTick();
				}
			});
		}
		if (!g_presentDisabled) {
			SafeOnPresent(a_sc);
		}
		return g_originalPresent(a_sc, a_sync, a_flags);
	}

	bool PatchPresentSlot(IDXGISwapChain* a_sc)
	{
		auto** vtable = *reinterpret_cast<void***>(a_sc);
		if (vtable[8] == reinterpret_cast<void*>(&HookedPresent)) {
			return true;
		}
		DWORD old = 0;
		if (!::VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			spdlog::error("VirtualProtect failed ({})", ::GetLastError());
			return false;
		}
		g_originalPresent = reinterpret_cast<PresentFn>(vtable[8]);
		vtable[8] = reinterpret_cast<void*>(&HookedPresent);
		::VirtualProtect(&vtable[8], sizeof(void*), old, &old);
		return true;
	}

	// ------------------------------------------------------------------ the frame hook (preferred)
	// The game's own end of frame (BSGraphics::Renderer::End, the call at +0x9), the place other interface mods hook.
	// Hooking DXGI's Present through the swap chain's vtable is never reached when something else (Community Shaders,
	// for one) takes over Present, and then nothing of this mod ran at all.
	using EndFn = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
	EndFn             g_originalEnd = nullptr;
	std::atomic<bool> g_framesSeen{ false };

	IDXGISwapChain* GameSwapChain()
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		return renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
	}

	void OnFrame()
	{
		if (!g_framesSeen.exchange(true)) {
			spdlog::info("Frame hook running");
		}
		if (!g_tickDisabled && !g_tickQueued.exchange(true)) {
			SKSE::GetTaskInterface()->AddTask([] {
				g_tickQueued = false;
				if (!g_tickDisabled) {
					SafeTick();
				}
			});
		}
		if (!g_presentDisabled) {
			if (auto* sc = GameSwapChain()) {
				SafeOnPresent(sc);
			}
		}
	}

	void HookedEnd(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4)
	{
		g_originalEnd(a1, a2, a3, a4);
		OnFrame();
	}

	bool SafeInstallFrameHook()
	{
		__try {
			REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(75461, 77246), 0x9 };
			if (*reinterpret_cast<const std::uint8_t*>(target.address()) != 0xE8) {
				spdlog::warn("Frame hook: the game's code isn't what was expected at {:X}", target.address());
				return false;
			}
			g_originalEnd = reinterpret_cast<EndFn>(SKSE::GetTrampoline().write_call<5>(target.address(), &HookedEnd));
			return g_originalEnd != nullptr;
		} __except (Overlay::SehFilter(GetExceptionInformation(), "InstallFrameHook")) {
			return false;
		}
	}

	bool SafeInstallPresentHook()
	{
		__try {
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto* sc = renderer ? reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain) : nullptr;
			return sc && PatchPresentSlot(sc);
		} __except (Overlay::SehFilter(GetExceptionInformation(), "InstallPresentHook")) {
			return false;
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Menu::Register();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			Performance::OnDataLoaded();
			if (SafeInstallFrameHook()) {
				spdlog::info("Frame hook installed (the game's end of frame)");
			} else if (SafeInstallPresentHook()) {
				spdlog::info("Present hook installed (the swap chain's; the frame hook couldn't go in)");
			} else {
				spdlog::error("Could not hook the game's frame - the instrument's interface will not appear");
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			{
				static bool s_input = false;
				if (!s_input) {
					s_input = true;
					if (!Overlay::InstallInputSink()) {
						spdlog::error("Could not register for input events");
					}
				}
			}
			Performance::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	spdlog::info("SkyrimSongs {} loaded", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."sv));
	LogEnvironment();
	SKSE::AllocTrampoline(64);
	Config::Load();
	if (auto* ser = SKSE::GetSerializationInterface()) {
		ser->SetUniqueID('SSNG');
		ser->SetSaveCallback(Performance::Save);
		ser->SetLoadCallback(Performance::Load);
		ser->SetRevertCallback(Performance::Revert);
	}
	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		spdlog::error("Could not register with the SKSE messaging interface");
		return false;
	}
	return true;
}
