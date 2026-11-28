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
		std::atomic<std::uint32_t> g_padButtons{ 0 };  // the controller's buttons held
		std::atomic<std::uint32_t> g_padDrop{ 0 };     // buttons the game doesn't get until they're let go (the hotkey's)

		std::uint32_t PadBit(int a_code)
		{
			using Pad = RE::BSWin32GamepadDevice::Key;
			return a_code == Pad::kLeftTrigger ? kPadLT : a_code == Pad::kRightTrigger ? kPadRT : static_cast<std::uint32_t>(a_code) & 0xFFFF;
		}
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
				if (dev == RE::INPUT_DEVICE::kGamepad) {
					const auto bit = PadBit(code);
					if (down) {
						g_padButtons |= bit;
					} else {
						g_padButtons &= ~bit;
						if (mode != Mode::Closed) {
							g_padDrop &= ~bit;  // let go while playing: nothing of it reaches the game anyway
						}
					}
				}
				if (mode == Mode::Closed && dev == RE::INPUT_DEVICE::kGamepad && down) {
					// the controller's hotkey: all its buttons held, this the last of them pressed. That press (and
					// its release) is kept from the game, so it doesn't open the favourites or the like
					const std::uint32_t want = ParsePadButtons(Config::Get().padHotkey), bit = PadBit(code);
					if (want && (want & bit) && (g_padButtons.load() & want) == want) {
						g_padDrop |= bit;
						spdlog::info("Controller hotkey pressed ({})", Config::Get().padHotkey);
						Performance::OnPadHotkey();
					}
					continue;
				}
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
				} else if (dev == RE::INPUT_DEVICE::kGamepad && mode == Mode::Playing) {
					Performance::OnPad(code, down);  // the controller is the instrument
				} else if (dev == RE::INPUT_DEVICE::kGamepad) {
					// the songbook: the left stick moves the pointer, A clicks, the D-pad scrolls, B / Back / Start close it
					using Pad = RE::BSWin32GamepadDevice::Key;
					if (code == Pad::kA || code == Pad::kRightTrigger) {
						g_padHeld = down;
						Push({ InputMsg::Kind::MouseButton, g_mx.load(), g_my.load(), 0, down });
					} else if (down && (code == Pad::kUp || code == Pad::kDown)) {
						Push({ InputMsg::Kind::Wheel, 0.0f, code == Pad::kUp ? 1.0f : -1.0f });
					} else if (down && (code == Pad::kB || code == Pad::kStart || code == Pad::kBack)) {
						Performance::OnKey(0x01, true);  // as Esc
						Performance::OnKey(0x01, false);
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
			if (g_padDrop.load() && a_events && *a_events) {
				// the controller hotkey's last button: kept from the game until it's let go
				std::array<std::pair<RE::InputEvent*, RE::InputEvent*>, 64> saved{};
				std::size_t     n = 0;
				RE::InputEvent* head = nullptr;
				RE::InputEvent** tail = &head;
				std::uint32_t   released = 0;
				for (auto* e = *a_events; e && n < saved.size();) {
					auto* next = e->next;
					saved[n++] = { e, next };
					bool keep = true;
					if (auto* btn = e->AsButtonEvent(); btn && e->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
						const auto bit = PadBit(static_cast<int>(btn->GetIDCode()));
						if (g_padDrop.load() & bit) {
							keep = false;
							if (btn->IsUp()) {
								released |= bit;
							}
						}
					}
					if (keep) {
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
				g_padDrop &= ~released;
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

	std::uint32_t PadHeld() { return g_padButtons.load(); }

	std::uint32_t ParsePadButtons(const std::string& a_text)
	{
		static const std::pair<const char*, std::uint32_t> kNames[] = { { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 },
			{ "LB", 0x0100 }, { "RB", 0x0200 }, { "LT", kPadLT }, { "RT", kPadRT }, { "LS", 0x0040 }, { "RS", 0x0080 }, { "L3", 0x0040 },
			{ "R3", 0x0080 }, { "BACK", 0x0020 }, { "VIEW", 0x0020 }, { "START", 0x0010 }, { "MENU", 0x0010 }, { "UP", 0x0001 },
			{ "DOWN", 0x0002 }, { "LEFT", 0x0004 }, { "RIGHT", 0x0008 } };
		std::uint32_t mask = 0;
		std::string   word;
		auto          take = [&]() {
            if (word.empty()) {
                return true;
            }
            for (const auto& [name, bit] : kNames) {
                if (word == name) {
                    mask |= bit;
                    word.clear();
                    return true;
                }
            }
            return false;
		};
		for (char c : a_text) {
			if (c == '+' || c == ' ' || c == ',') {
				if (!take()) {
					return 0;
				}
			} else {
				word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
		}
		return take() ? mask : 0;
	}

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
		if (sc != g_sc) {
			// the first swap chain, or a new one: an upscaler or frame generation can put its own in place of the
			// game's. Follow the one that's presenting (on the game's window), or the interface goes where nobody sees.
			static HWND s_window = nullptr;
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow || (s_window && desc.OutputWindow != s_window)) {
				return;
			}
			ID3D11Device* scDev = nullptr;
			if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&scDev))) || !scDev) {
				static bool s_said = false;
				if (!s_said) {
					s_said = true;
					spdlog::warn("A swap chain without a Direct3D 11 device presents; not drawing on it");
				}
				return;
			}
			// Draw with the game's own device and context, as the game (and other interface mods) do. With ENB the
			// swap chain hands back the device under ENB's wrapper; drawing on that goes past ENB, which then covers it.
			ID3D11Device*        dev = scDev;
			ID3D11DeviceContext* gameCtx = nullptr;
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& rd = renderer->GetRuntimeData();
				if (rd.forwarder && rd.context) {
					dev = reinterpret_cast<ID3D11Device*>(rd.forwarder);
					gameCtx = reinterpret_cast<ID3D11DeviceContext*>(rd.context);
					dev->AddRef();
					scDev->Release();
				}
			}
			const bool wrapped = gameCtx && reinterpret_cast<void*>(dev) != reinterpret_cast<void*>(scDev);
			if (!g_sc) {
				g_dev = dev;
				if (gameCtx) {
					g_ctx = gameCtx;
					g_ctx->AddRef();
				} else {
					g_dev->GetImmediateContext(&g_ctx);
				}
				spdlog::info("Attached to the game's swap chain ({}x{}, format {}), drawing with {}{}", desc.BufferDesc.Width, desc.BufferDesc.Height,
					static_cast<int>(desc.BufferDesc.Format), gameCtx ? "the game's device" : "the swap chain's device",
					wrapped ? " (the swap chain's is another: ENB or a proxy is in between)" : "");
			} else {
				static int s_changes = 0;
				if (++s_changes <= 5) {
					spdlog::info("The swap chain changed ({}x{}, format {}{}): drawing on the new one", desc.BufferDesc.Width, desc.BufferDesc.Height,
						static_cast<int>(desc.BufferDesc.Format), dev != g_dev ? ", another device" : "");
				}
				if (dev != g_dev) {
					if (g_imguiReady) {
						ImGui_ImplDX11_Shutdown();
					}
					if (g_ctx) {
						g_ctx->Release();
					}
					g_dev->Release();
					g_dev = dev;
					if (gameCtx) {
						g_ctx = gameCtx;
						g_ctx->AddRef();
					} else {
						g_dev->GetImmediateContext(&g_ctx);
					}
					if (g_imguiReady && (!ImGui_ImplDX11_Init(g_dev, g_ctx) || !ImGui_ImplDX11_CreateDeviceObjects())) {
						g_imguiFailed = true;
						spdlog::error("ImGui failed to move to the new device");
					}
				} else {
					dev->Release();
				}
			}
			s_window = desc.OutputWindow;
			g_sc = sc;
			g_screenW = static_cast<float>(desc.BufferDesc.Width);
			g_screenH = static_cast<float>(desc.BufferDesc.Height);
		}
		if (g_mode.load() == Mode::Closed) {
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
			static bool s_said = false;
			if (!s_said) {
				s_said = true;
				spdlog::warn("Can't get the swap chain's back buffer; the interface can't be drawn");
			}
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

		// An upscaler (Skyrim Upscaler, DLSS/FSR through the game's dynamic resolution) has the frame drawn into the
		// top-left part of the buffer at the render scale, and stretches that part to the screen afterwards. Drawn at
		// full size, the interface came out enlarged and cut off on the right and bottom: lay it out at full size,
		// then draw it shrunk by the same scale, so the stretch brings it back to where it belongs.
		if (Config::Get().upscalerFix) {
			float rx = 1.0f, ry = 1.0f;
			if (auto* state = RE::BSGraphics::State::GetSingleton()) {
				rx = state->GetRuntimeData().dynamicResolutionWidthRatio;
				ry = state->GetRuntimeData().dynamicResolutionHeightRatio;
			}
			const bool valid = rx >= 0.25f && rx <= 1.0f && ry >= 0.25f && ry <= 1.0f;
			if (valid && (rx < 0.995f || ry < 0.995f)) {
				static float s_saidX = 0.0f, s_saidY = 0.0f;
				if (std::abs(rx - s_saidX) > 0.01f || std::abs(ry - s_saidY) > 0.01f) {
					s_saidX = rx;
					s_saidY = ry;
					spdlog::info("Render scale {:.2f} x {:.2f} (an upscaler): the interface is drawn to match", rx, ry);
				}
				auto* dd = ImGui::GetDrawData();
				for (int i = 0; i < dd->CmdListsCount; ++i) {
					for (auto& v : dd->CmdLists[i]->VtxBuffer) {
						v.pos.x *= rx;
						v.pos.y *= ry;
					}
				}
				dd->ScaleClipRects(ImVec2(rx, ry));
			}
		}

		// a "typeless" back buffer (upscalers and frame generation use them) needs the format named for a view
		D3D11_RENDER_TARGET_VIEW_DESC  rd{};
		D3D11_RENDER_TARGET_VIEW_DESC* prd = nullptr;
		{
			DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
			switch (bd.Format) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				typed = DXGI_FORMAT_R8G8B8A8_UNORM;
				break;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				typed = DXGI_FORMAT_B8G8R8A8_UNORM;
				break;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				typed = DXGI_FORMAT_R10G10B10A2_UNORM;
				break;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				typed = DXGI_FORMAT_R16G16B16A16_FLOAT;
				break;
			default:
				break;
			}
			if (typed != DXGI_FORMAT_UNKNOWN) {
				rd.Format = typed;
				rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
				prd = &rd;
			}
		}
		ID3D11RenderTargetView* rtv = nullptr;
		const HRESULT           hr = g_dev->CreateRenderTargetView(back, prd, &rtv);
		back->Release();
		if (FAILED(hr) || !rtv) {
			static bool s_said = false;
			if (!s_said) {
				s_said = true;
				spdlog::warn("Can't draw on the back buffer (format {}, error {:08X}); the interface won't show", static_cast<int>(bd.Format),
					static_cast<std::uint32_t>(hr));
			}
			return;
		}
		// draw, then put back what was bound: whatever presents after us (ENB, an upscaler) gets the state it left
		ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* oldDsv = nullptr;
		g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
		UINT           nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		g_ctx->RSGetViewports(&nvp, oldVp);
		g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		rtv->Release();
		g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
		g_ctx->RSSetViewports(nvp, oldVp);
		for (auto* v : oldRtv) {
			if (v) {
				v->Release();
			}
		}
		if (oldDsv) {
			oldDsv->Release();
		}
	}
}
