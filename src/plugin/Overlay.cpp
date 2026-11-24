// SkyrimSongs - play the lute and flute in Skyrim.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Overlay.h"

#include "Config.h"
#include "Performance.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace Overlay
{
	namespace
	{
		struct InputMsg
		{
			enum class Kind
			{
				MouseMove,
				MouseButton,
				Wheel
			} kind;
			float x = 0.0f, y = 0.0f;
			int   button = 0;
			bool  down = false;
		};
		std::mutex            g_inputMtx;
		std::vector<InputMsg> g_input;  // for ImGui (drained in Present)

		std::atomic<Mode>  g_mode{ Mode::Closed };
		std::atomic<bool>  g_inputDisabled{ false };
		std::atomic<float> g_stick[2]{};
		std::atomic<bool>  g_padHeld{ false };
		std::atomic<float> g_mx{ 0.0f }, g_my{ 0.0f };
		std::atomic<float> g_screenW{ 1920.0f }, g_screenH{ 1080.0f };
		std::atomic<bool>  g_center{ true };

		bool                 g_imguiReady = false;
		bool                 g_imguiFailed = false;
		IDXGISwapChain*      g_sc = nullptr;
		ID3D11Device*        g_dev = nullptr;
		ID3D11DeviceContext* g_ctx = nullptr;
		std::chrono::steady_clock::time_point g_lastFrame{};

		void Push(const InputMsg& a_m)
		{
			std::lock_guard lk(g_inputMtx);
			if (g_input.size() < 1024) {
				g_input.push_back(a_m);
			}
		}

		// What still reaches the game while you play: looking about (the mouse, the sticks, the wheel's zoom)
		bool PassWhilePlaying(RE::InputEvent* e)
		{
			const auto type = e->GetEventType();
			if (type == RE::INPUT_EVENT_TYPE::kMouseMove || type == RE::INPUT_EVENT_TYPE::kThumbstick) {
				return true;
			}
			if (auto* btn = e->AsButtonEvent(); btn && e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
				const auto code = btn->GetIDCode();
				return code == RE::BSWin32MouseDevice::Key::kWheelUp || code == RE::BSWin32MouseDevice::Key::kWheelDown;
			}
			return false;
		}

		void HandleInput(RE::InputEvent* const* a_event)
		{
			if (!a_event) {
				return;
			}
			const Mode mode = g_mode.load();
			for (auto* e = *a_event; e; e = e->next) {
				const auto type = e->GetEventType();
				if (mode == Mode::Book && type == RE::INPUT_EVENT_TYPE::kMouseMove) {
					auto*       mm = static_cast<RE::MouseMoveEvent*>(e);
					g_mx = std::clamp(g_mx.load() + static_cast<float>(mm->mouseInputX), 0.0f, g_screenW.load() - 1.0f);
					g_my = std::clamp(g_my.load() + static_cast<float>(mm->mouseInputY), 0.0f, g_screenH.load() - 1.0f);
					Push({ InputMsg::Kind::MouseMove, g_mx.load(), g_my.load() });
					continue;
				}
				if (mode == Mode::Book && type == RE::INPUT_EVENT_TYPE::kThumbstick) {
					auto* ts = static_cast<RE::ThumbstickEvent*>(e);
					if (ts->IsLeft()) {
						g_stick[0] = ts->xValue;
						g_stick[1] = ts->yValue;
					}
					continue;
				}
				auto* btn = e->AsButtonEvent();
				if (!btn) {
					continue;
				}
				const auto code = static_cast<int>(btn->GetIDCode());
				const bool down = btn->IsDown();
				const bool up = btn->IsUp();
				if (!down && !up) {
					continue;
				}
				if (Config::Get().logInput) {
					spdlog::info("[input] device {} code {:X} {}", static_cast<int>(e->GetDevice()), code, down ? "down" : "up");
				}
				const auto dev = e->GetDevice();
				if (mode == Mode::Closed) {
					if (dev == RE::INPUT_DEVICE::kKeyboard && down) {
						// the first few keys each session are logged, so "the hotkey does nothing" can be told apart from
						// "no keys reach the mod"
						static int s_logged = 0;
						if (s_logged < 6 && !Config::Get().logInput) {
							++s_logged;
							spdlog::info("[input] key 0x{:X} down (the hotkey is 0x{:X})", code, Config::Get().hotkey);
						}
						Performance::OnHotkey(code);
					}
					continue;
				}
				if (dev == RE::INPUT_DEVICE::kKeyboard) {
					Performance::OnKey(code, down);
				} else if (dev == RE::INPUT_DEVICE::kMouse && mode == Mode::Book) {
					using MouseKey = RE::BSWin32MouseDevice::Key;
					if (code == MouseKey::kWheelUp || code == MouseKey::kWheelDown) {
						if (down) {
							Push({ InputMsg::Kind::Wheel, 0.0f, code == MouseKey::kWheelUp ? 1.0f : -1.0f });
						}
					} else if (code <= 2) {
						Push({ InputMsg::Kind::MouseButton, g_mx.load(), g_my.load(), code, down });
					}
				} else if (dev == RE::INPUT_DEVICE::kGamepad) {
					using Pad = RE::BSWin32GamepadDevice::Key;
					if (mode == Mode::Book && (code == Pad::kA || code == Pad::kRightTrigger)) {
						g_padHeld = down;
						Push({ InputMsg::Kind::MouseButton, g_mx.load(), g_my.load(), 0, down });
					} else if (down && (code == Pad::kB || code == Pad::kStart)) {
						Performance::OnKey(0x01, true);  // as Esc
						Performance::OnKey(0x01, false);
					} else if (down && code == Pad::kBack) {
						Performance::OnKey(0x0F, true);  // as Tab: the songbook
						Performance::OnKey(0x0F, false);
					}
				}
			}
		}

		void SafeHandleInput(RE::InputEvent* const* a_event)
		{
			__try {
				HandleInput(a_event);
			} __except (SehFilter(GetExceptionInformation(), "HandleInput")) {
				g_inputDisabled = true;
			}
		}

		using DispatchFn = void (*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent* const*);
		DispatchFn g_origDispatch = nullptr;

		std::atomic<std::int64_t> g_hookInputAt{ 0 };
		std::int64_t              NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void DispatchHook(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events)
		{
			static auto* s_mgr = static_cast<RE::BSTEventSource<RE::InputEvent*>*>(RE::BSInputDeviceManager::GetSingleton());
			if (a_dispatcher != s_mgr) {
				static bool s_warned = false;
				if (!s_warned) {
					s_warned = true;
					spdlog::warn("Input hook: this call isn't the input manager's in this game version; using the event sink");
				}
				g_origDispatch(a_dispatcher, a_events);
				return;
			}
			if (a_events && *a_events) {
				if (g_hookInputAt.exchange(NowMs()) == 0) {
					spdlog::info("Input hook: events arriving");
				}
			}
			if (!g_inputDisabled) {
				SafeHandleInput(a_events);
			}
			const Mode mode = g_mode.load();
			if (mode == Mode::Book) {
				static RE::InputEvent* s_none[1] = { nullptr };
				s_none[0] = nullptr;
				g_origDispatch(a_dispatcher, s_none);
				return;
			}
			if (mode == Mode::Playing && a_events && *a_events) {
				// relink the frame's events so only the looking-about ones go on, then put the list back as it was
				std::array<std::pair<RE::InputEvent*, RE::InputEvent*>, 64> saved{};
				std::size_t     n = 0;
				RE::InputEvent* head = nullptr;
				RE::InputEvent** tail = &head;
				for (auto* e = *a_events; e && n < saved.size();) {
					auto* next = e->next;
					saved[n++] = { e, next };
					if (PassWhilePlaying(e)) {
						*tail = e;
						tail = &e->next;
					}
					e = next;
				}
				*tail = nullptr;
				RE::InputEvent* list[1] = { head };
				g_origDispatch(a_dispatcher, list);
				for (std::size_t i = 0; i < n; ++i) {
					saved[i].first->next = saved[i].second;
				}
				return;
			}
			g_origDispatch(a_dispatcher, a_events);
		}

		bool InstallDispatchHook()
		{
			// BSInputDeviceManager: the SendEvent call inside the input polling function (SE 67315, AE 68617)
			REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617), 0x7B };
			const auto*                     op = reinterpret_cast<const std::uint8_t*>(target.address());
			if (*op != 0xE8) {
				spdlog::warn("Input hook: expected a call at {:X}, found {:02X}", target.address(), *op);
				return false;
			}
			g_origDispatch = reinterpret_cast<DispatchFn>(SKSE::GetTrampoline().write_call<5>(target.address(), &DispatchHook));
			return g_origDispatch != nullptr;
		}

		bool SafeInstallDispatchHook()
		{
			__try {
				return InstallDispatchHook();
			} __except (SehFilter(GetExceptionInformation(), "InstallDispatchHook")) {
				return false;
			}
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				// the hook reads the input when it works; this is the fallback (the keys then also reach the game)
				if (!g_inputDisabled && NowMs() - g_hookInputAt.load() > 500) {
					SafeHandleInput(a_event);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void DrainInput()
		{
			std::vector<InputMsg> msgs;
			{
				std::lock_guard lk(g_inputMtx);
				msgs.swap(g_input);
			}
			auto& io = ImGui::GetIO();
			for (const auto& m : msgs) {
				switch (m.kind) {
				case InputMsg::Kind::MouseMove:
					io.AddMousePosEvent(m.x, m.y);
					break;
				case InputMsg::Kind::MouseButton:
					io.AddMousePosEvent(m.x, m.y);
					io.AddMouseButtonEvent(m.button, m.down);
					break;
				case InputMsg::Kind::Wheel:
					io.AddMouseWheelEvent(0.0f, m.y);
					break;
				}
			}
			static constexpr int kVk[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
			for (int b = 0; b < 3; ++b) {
				if (io.MouseDown[b] && !(b == 0 && g_padHeld) && (::GetAsyncKeyState(kVk[b]) & 0x8000) == 0) {
					io.AddMouseButtonEvent(b, false);
				}
			}
		}

		bool InitImGui()
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			Performance::SetupUi(g_screenH.load());
			return ImGui_ImplDX11_Init(g_dev, g_ctx);
		}
	}

	void SetMode(Mode a_mode)
	{
		const Mode old = g_mode.exchange(a_mode);
		if (old == a_mode) {
			return;
		}
		{
			std::lock_guard lk(g_inputMtx);
			g_input.clear();
		}
		g_stick[0] = g_stick[1] = 0.0f;
		if (a_mode == Mode::Book) {
			g_center = true;
		}
	}

	Mode GetMode() { return g_mode.load(); }

	void ScreenSize(float& a_w, float& a_h)
	{
		a_w = g_screenW.load();
		a_h = g_screenH.load();
	}

	int SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where)
	{
		const auto* rec = a_info ? a_info->ExceptionRecord : nullptr;
		const auto  addr = rec ? reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) : 0;
		HMODULE     mod = nullptr;
		char        modPath[MAX_PATH]{};
		if (addr && ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(addr), &mod)) {
			::GetModuleFileNameA(mod, modPath, MAX_PATH);
		}
		const auto offset = mod ? addr - reinterpret_cast<std::uintptr_t>(mod) : addr;
		spdlog::critical("CRASH CAUGHT in {}: exception 0x{:08X} at {}+0x{:X} - this part is now switched off so the game keeps running",
			a_where, rec ? static_cast<std::uint32_t>(rec->ExceptionCode) : 0u, std::filesystem::path(modPath).filename().string(), offset);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool InstallInputSink()
	{
		if (SafeInstallDispatchHook()) {
			spdlog::info("Input hook installed: while you play, the keyboard is the instrument");
		} else {
			spdlog::warn("Input hook not installed: reading input through an event sink (the keys will also reach the game)");
		}
		static InputSink sink;
		auto*            mgr = RE::BSInputDeviceManager::GetSingleton();
		if (!mgr) {
			return false;
		}
		mgr->AddEventSink<RE::InputEvent*>(&sink);
		return true;
	}

	void OnPresent(IDXGISwapChain* sc)
	{
		if (!g_sc) {
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {
				return;
			}
			if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) {
				return;
			}
			g_dev->GetImmediateContext(&g_ctx);
			g_sc = sc;
			g_screenW = static_cast<float>(desc.BufferDesc.Width);
			g_screenH = static_cast<float>(desc.BufferDesc.Height);
			spdlog::info("Attached to the game's swap chain ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
		}
		if (sc != g_sc || g_mode.load() == Mode::Closed) {
			return;
		}
		if (!g_imguiReady && !g_imguiFailed) {
			if (!InitImGui() || !ImGui_ImplDX11_CreateDeviceObjects()) {
				g_imguiFailed = true;
				spdlog::error("ImGui failed to initialise");
				return;
			}
			g_imguiReady = true;
		}
		if (g_imguiFailed) {
			return;
		}

		ID3D11Texture2D* back = nullptr;
		if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		g_screenW = static_cast<float>(bd.Width);
		g_screenH = static_cast<float>(bd.Height);

		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
		const auto  now = std::chrono::steady_clock::now();
		const float dt = g_lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - g_lastFrame).count() : 1.0f / 60.0f;
		g_lastFrame = now;
		io.DeltaTime = std::clamp(dt, 1.0f / 1000.0f, 0.5f);
		const bool book = g_mode.load() == Mode::Book;
		if (book && g_center.exchange(false)) {
			g_mx = io.DisplaySize.x * 0.5f;
			g_my = io.DisplaySize.y * 0.5f;
			io.AddMousePosEvent(g_mx.load(), g_my.load());
		}
		if (book) {
			// the left stick moves the pointer
			const float x = g_stick[0], y = g_stick[1], m = std::sqrt(x * x + y * y);
			if (m > 0.15f) {
				const float g = (m - 0.15f) / 0.85f;
				const float k = 1500.0f * (io.DisplaySize.y / 1080.0f) * io.DeltaTime * g * g;
				g_mx = std::clamp(g_mx.load() + x / m * k, 0.0f, io.DisplaySize.x - 1.0f);
				g_my = std::clamp(g_my.load() - y / m * k, 0.0f, io.DisplaySize.y - 1.0f);
				Push({ InputMsg::Kind::MouseMove, g_mx.load(), g_my.load() });
			}
		}
		DrainInput();
		io.MouseDrawCursor = book;
		if (!book) {
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // no hover while you play
		}

		// the interface size changed in the settings: rebuild the fonts
		{
			static float s_applied = Config::Get().uiScale;
			if (Config::Get().uiScale != s_applied) {
				s_applied = Config::Get().uiScale;
				io.Fonts->Clear();
				Performance::SetupUi(g_screenH.load());
				ImGui_ImplDX11_InvalidateDeviceObjects();
			}
		}
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		Performance::DrawUi();
		ImGui::Render();

		ID3D11RenderTargetView* rtv = nullptr;
		g_dev->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
		if (rtv) {
			g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			rtv->Release();
		}
	}
}
