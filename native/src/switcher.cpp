#include "switcher_ipc.h"
#include "switcher_model.h"
#include "switcher_layout.h"
#include "preview.h"
#include "resource.h"
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <optional>
#include <windowsx.h>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <memory>
#include <thread>
using namespace bridge;
using namespace switcher;
namespace
{
	constexpr UINT keyMessage = WM_APP + 51, pollTimer = 51, trayMessage = WM_APP + 52;
	UINT taskbarCreated = 0;
	LRESULT CALLBACK keyboard(int code, WPARAM wParam, LPARAM lParam);
	constexpr int pageSize = 24;
	HWND overlay = nullptr;
	std::atomic_bool enabled{true}, capturing{false}, available{false}, fullscreen{false};
	std::atomic<ULONGLONG> heartbeat{0};
	std::ofstream diagnostics;
	void log(const std::string& text)
	{
		if (diagnostics.tellp() > 1024 * 1024) { return; }
		diagnostics << GetTickCount64() << ' ' << text << std::endl;
	}
	bool focusWindow(HWND target)
	{
		const HWND foreground = GetForegroundWindow();
		const DWORD currentThread = GetCurrentThreadId();
		const DWORD foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
		const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);
		log("focus target=" + std::to_string(reinterpret_cast<std::uint64_t>(target)) + " current=" + std::to_string(reinterpret_cast<std::uint64_t>(foreground)));
		// Attach only during this explicit user handoff. Joining the destination
		// queue as well lets activation complete before the popup is hidden.
		std::vector<DWORD> attached; attached.reserve(2);
		for (const auto [hwnd, thread] : {std::pair{foreground, foregroundThread}, std::pair{target, targetThread}})
		{
			if (thread && thread != currentThread && std::find(attached.begin(), attached.end(), thread) == attached.end()
				&& !IsHungAppWindow(hwnd) && AttachThreadInput(currentThread, thread, TRUE)) { attached.push_back(thread); }
		}
		if (!attached.empty()) { BringWindowToTop(target); SetActiveWindow(target); }
		const bool activated = SetForegroundWindow(target) != FALSE || GetForegroundWindow() == target;
		DWORD detachError = ERROR_SUCCESS;
		for (auto it = attached.rbegin(); it != attached.rend(); ++it)
		{
			if (!AttachThreadInput(currentThread, *it, FALSE)) { detachError = GetLastError(); }
		}
		if (detachError) { throw winrt::hresult_error(HRESULT_FROM_WIN32(detachError)); }
		return activated;
	}
	std::uint64_t now()
	{
		FILETIME time{}; GetSystemTimeAsFileTime(&time);
		ULARGE_INTEGER value{}; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
		return value.QuadPart / 10000 - 11644473600000ull;
	}
	struct Operation
	{
		winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
		OVERLAPPED overlapped{};
		bool pending = false;
		Operation() { winrt::check_bool(static_cast<bool>(event)); overlapped.hEvent = event.get(); }
		void reset() { ResetEvent(event.get()); overlapped = {}; overlapped.hEvent = event.get(); pending = false; }
	};
	struct PendingActivation { int tab; HWND source; ULONGLONG created; std::uint64_t sequence; };
	struct Client
	{
		std::uint64_t id;
		winrt::handle pipe;
		Operation read, write;
		std::array<char, 65536> chunk{};
		std::vector<char> input;
		std::deque<std::vector<char>> output;
		DWORD written = 0;
		ULONGLONG writeStarted = 0, frameStarted = 0;
		BrowserProcess browser;
		HICON appIcon = nullptr;
		std::optional<bool> previewsEnabled;
		std::map<int, TabData> tabs;
		std::map<int, PreviewPixels> previews;
		std::deque<int> previewOrder;
		std::map<int, HWND> windows;
		std::deque<PendingActivation> pending;
		int active = -1;
		bool focused = false, hasSnapshot = false;
		Client(std::uint64_t number, Endpoint& endpoint) : id(number)
		{
			pipe.attach(CreateNamedPipeW(endpoint.pipe().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 9, 65536, 65536, 0, &endpoint.security));
			winrt::check_bool(pipe.get() != INVALID_HANDLE_VALUE);
			if (!ConnectNamedPipe(pipe.get(), &read.overlapped))
			{
				const auto error = GetLastError();
				if (error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED) { winrt::throw_last_error(); }
				read.pending = error == ERROR_IO_PENDING;
			}
		}
		~Client()
		{
			if (appIcon) { DestroyIcon(appIcon); }
			if (pipe && pipe.get() != INVALID_HANDLE_VALUE)
			{
				CancelIoEx(pipe.get(), nullptr);
				DWORD ignored = 0;
				if (read.pending) { GetOverlappedResult(pipe.get(), &read.overlapped, &ignored, TRUE); }
				if (write.pending) { GetOverlappedResult(pipe.get(), &write.overlapped, &ignored, TRUE); }
				DisconnectNamedPipe(pipe.get());
			}
		}
		void send(const JsonObject& message)
		{
			if (output.size() >= 64) { throw winrt::hresult_error(E_FAIL, L"Client output queue exceeded"); }
			output.push_back(frame(message));
		}
		void flush()
		{
			for (int budget = 0; budget < 8 && !output.empty(); ++budget)
			{
				DWORD count = 0;
				if (write.pending)
				{
					if (!GetOverlappedResult(pipe.get(), &write.overlapped, &count, FALSE))
					{
						if (GetLastError() != ERROR_IO_INCOMPLETE) { winrt::throw_last_error(); }
						if (GetTickCount64() - writeStarted > 10000) { throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
						return;
					}
					write.pending = false;
				}
				else
				{
					write.reset();
					if (!WriteFile(pipe.get(), output.front().data() + written, static_cast<DWORD>(output.front().size()) - written, &count, &write.overlapped))
					{
						if (GetLastError() != ERROR_IO_PENDING) { winrt::throw_last_error(); }
						write.pending = true; writeStarted = GetTickCount64(); return;
					}
				}
				if (!count) { throw winrt::hresult_error(E_FAIL); }
				written += count;
				if (written == output.front().size()) { output.pop_front(); written = 0; }
			}
		}
	};
	struct Item { Key key; std::wstring title; HWND hwnd = nullptr; };
	struct FrameBuffer
	{
		HDC dc = nullptr;
		HBITMAP bitmap = nullptr;
		HGDIOBJ original = nullptr;
		void* pixels = nullptr;
		int width = 0, height = 0;
		std::uint64_t allocations = 0;
		~FrameBuffer() { reset(); }
		void reset()
		{
			if (dc && original) { SelectObject(dc, original); }
			if (bitmap) { DeleteObject(bitmap); }
			if (dc) { DeleteDC(dc); }
			dc = nullptr; bitmap = nullptr; original = nullptr; pixels = nullptr; width = height = 0;
		}
		void resize(HDC target, int w, int h)
		{
			if (dc && w == width && h == height) { return; }
			reset(); dc = CreateCompatibleDC(target); winrt::check_bool(dc != nullptr);
			BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w;
			info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
			bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0); winrt::check_bool(bitmap != nullptr);
			original = SelectObject(dc, bitmap); winrt::check_bool(original != nullptr && original != HGDI_ERROR);
			width = w; height = h; ++allocations;
		}
	};
	struct App
	{
		Endpoint endpoint;
		std::vector<std::unique_ptr<Client>> clients;
		std::unique_ptr<Client> listener;
		std::uint64_t nextClient = 1, selectionSequence = 0;
		Recent recent;
		std::vector<Item> items;
		std::vector<HTHUMBNAIL> thumbnails;
		std::set<size_t> liveSlots;
		size_t thumbnailPage = SIZE_MAX;
		std::uint64_t thumbnailBuilds = 0, preparedFrames = 0;
		FrameBuffer frame;
		bool preparedPaint = false, preparedWhileHidden = false;
		winrt::com_ptr<IVirtualDesktopManager> desktops;
		size_t selected = 0;
		int hovered = -1;
		HWND previous = nullptr, frameOwner = nullptr;
		int width = 1120, height = 510;
		std::vector<RECT> cardRects;
		bool glass = false;
		winrt::Windows::System::DispatcherQueueController dispatcher{nullptr};
		winrt::Windows::UI::Composition::Compositor compositor{nullptr};
		winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget compositionTarget{nullptr};
		winrt::Windows::UI::Composition::CompositionGraphicsDevice graphicsDevice{nullptr};
		winrt::Windows::UI::Composition::CompositionDrawingSurface contentSurface{nullptr};
		winrt::Windows::UI::Composition::SpriteVisual contentVisual{nullptr};
		winrt::com_ptr<ID2D1Bitmap1> contentBitmap;
		int surfaceWidth = 0, surfaceHeight = 0;
		std::uint64_t surfaceAllocations = 0;
		RECT workArea{};
		UINT dpi = 96;
		bool dark = true;
		bool demo = false, test = false, showing = false;
		ULONGLONG commitDue = 0;
		ULONGLONG emptySince = GetTickCount64();
		HFONT font = nullptr;
		HWINEVENTHOOK foregroundHook = nullptr;
		std::thread keyboardThread;
		DWORD keyboardThreadId = 0;
		winrt::handle quitEvent, pauseEvent, resumeEvent;
		ULONGLONG quitDue = 0;
		NOTIFYICONDATAW tray{sizeof(tray)};
		bool trayAdded = false;
		std::wstring trayStatus;
		void stopKeyboard()
		{
			capturing = false;
			if (keyboardThread.joinable())
			{
				winrt::check_bool(PostThreadMessageW(keyboardThreadId, WM_QUIT, 0, 0));
				keyboardThread.join(); keyboardThreadId = 0;
				log("keyboard hook removed");
			}
		}
		void startKeyboard()
		{
			if (test || keyboardThread.joinable()) { return; }
			winrt::handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
			winrt::check_bool(static_cast<bool>(ready));
			std::atomic<DWORD> error{0};
			keyboardThread = std::thread([&] {
				MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
				keyboardThreadId = GetCurrentThreadId();
				HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, GetModuleHandleW(nullptr), 0);
				if (!hook) { error = GetLastError(); }
				SetEvent(ready.get());
				if (hook)
				{
					while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
					if (!UnhookWindowsHookEx(hook)) { log("keyboard hook removal failed"); }
				}
			});
			winrt::check_bool(WaitForSingleObject(ready.get(), INFINITE) == WAIT_OBJECT_0);
			if (error) { keyboardThread.join(); throw winrt::hresult_error(HRESULT_FROM_WIN32(error.load())); }
			log("keyboard hook installed");
		}
		void setEnabled(bool value)
		{
			enabled = value; hide(false);
			if (value) { startKeyboard(); } else { stopKeyboard(); }
			updateTray();
		}
		void updateTray(bool recreate = false)
		{
			if (test || demo) { return; }
			if (recreate) { trayAdded = false; }
			const auto status = !enabled ? L"Paused - keyboard hook removed" : !available ? L"Waiting for Chrome" : L"Running";
			if (trayAdded && trayStatus == status) { return; }
			tray.hWnd = overlay; tray.uID = 1;
			tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
			tray.uCallbackMessage = trayMessage;
			tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_CHROME_ALT_TAB));
			const std::wstring tip = std::wstring(L"Chrome Alt+Tab: ") + status;
			wcscpy_s(tray.szTip, tip.c_str());
			if (!Shell_NotifyIconW(trayAdded ? NIM_MODIFY : NIM_ADD, &tray)) { return; }
			trayAdded = true; trayStatus = status;
			tray.uVersion = NOTIFYICON_VERSION_4;
			if (!Shell_NotifyIconW(NIM_SETVERSION, &tray)) { log("tray icon version unavailable"); }
		}
		void requestQuit()
		{
			if (quitDue) { return; }
			setEnabled(false);
			// Stop reconnection in each extension for this Chrome session. A user
			// can explicitly reconnect in its popup, or restart Chrome normally.
			for (auto& connection : clients)
			{
				try { connection->send(messageFor(L"paused")); connection->flush(); } catch (...) { log("quit notification failed"); }
			}
			quitDue = GetTickCount64() + 250;
		}
		void trayMenu()
		{
			hide(false);
			HMENU menu = CreatePopupMenu(); winrt::check_bool(menu != nullptr);
			try
			{
				const std::wstring caption = L"Chrome Alt+Tab " + std::wstring(winrt::to_hstring(CHROME_ALT_TAB_VERSION)) + L" (preview)";
				winrt::check_bool(AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, caption.c_str()));
				winrt::check_bool(AppendMenuW(menu, MF_SEPARATOR, 0, nullptr));
				winrt::check_bool(AppendMenuW(menu, MF_STRING, 1, enabled ? L"Pause (release keyboard hook)" : L"Resume"));
				winrt::check_bool(AppendMenuW(menu, MF_STRING, 2, L"Help, installation and removal"));
				winrt::check_bool(AppendMenuW(menu, MF_STRING, 3, L"Uninstall..."));
				winrt::check_bool(AppendMenuW(menu, MF_SEPARATOR, 0, nullptr));
				winrt::check_bool(AppendMenuW(menu, MF_STRING, 4, L"Quit (resume from Chrome extension)"));
				POINT point{}; winrt::check_bool(GetCursorPos(&point));
				SetForegroundWindow(frameOwner);
				const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, frameOwner, nullptr);
				PostMessageW(frameOwner, WM_NULL, 0, 0);
				if (choice == 1) { setEnabled(!enabled); }
				else if (choice == 2 || choice == 3)
				{
					const auto root = std::filesystem::path(executable()).parent_path();
					const auto file = root / (choice == 2 ? L"QUICKSTART.html" : L"uninstall.ps1");
					if (!std::filesystem::is_regular_file(file)) { MessageBoxW(nullptr, L"This is a developer build. See README.md for setup and removal instructions.", L"Chrome Alt+Tab", MB_OK | MB_ICONINFORMATION); }
					else if (choice == 2)
					{
						if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) { throw winrt::hresult_error(E_FAIL, L"Cannot open help"); }
					}
					else
					{
						wchar_t system[MAX_PATH]{}; winrt::check_bool(GetSystemDirectoryW(system, MAX_PATH));
						const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
						const auto arguments = L"-NoProfile -ExecutionPolicy Bypass -File \"" + file.wstring() + L"\"";
						if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", powershell.c_str(), arguments.c_str(), nullptr, SW_HIDE)) <= 32) { throw winrt::hresult_error(E_FAIL, L"Cannot start uninstaller"); }
					}
				}
				else if (choice == 4) { requestQuit(); }
			}
			catch (...) { DestroyMenu(menu); throw; }
			DestroyMenu(menu);
		}
		App(bool demonstration, bool testing) : demo(demonstration), test(testing) {}
		~App()
		{
			enabled = false; capturing = false; available = false;
			if (keyboardThread.joinable()) { PostThreadMessageW(keyboardThreadId, WM_QUIT, 0, 0); keyboardThread.join(); }
			if (foregroundHook) { UnhookWinEvent(foregroundHook); }
			if (trayAdded) { Shell_NotifyIconW(NIM_DELETE, &tray); }
			clearThumbnails();
			if (font) { DeleteObject(font); }
			if (frameOwner) { DestroyWindow(frameOwner); }
		}
		Client* client(std::uint64_t id)
		{
			for (auto& connection : clients) { if (connection->id == id) { return connection.get(); } }
			return nullptr;
		}
		void clearThumbnails() { for (auto thumbnail : thumbnails) { DwmUnregisterThumbnail(thumbnail); } thumbnails.clear(); liveSlots.clear(); thumbnailPage = SIZE_MAX; }
		bool normalWindow(HWND hwnd)
		{
			if (!IsWindowVisible(hwnd) || hwnd == overlay || hwnd == GetShellWindow()) { return false; }
			const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			if (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) { return false; }
			if (GetWindow(hwnd, GW_OWNER) && !(style & WS_EX_APPWINDOW)) { return false; }
			DWORD cloaked = 0;
			if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) { return false; }
			BOOL current = TRUE;
			if (desktops && SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &current)) && !current) { return false; }
			return GetWindowTextLengthW(hwnd) > 0;
		}
		std::wstring title(HWND hwnd) { wchar_t text[2048]{}; GetWindowTextW(hwnd, text, static_cast<int>(std::size(text))); return text; }
		HWND resolveWindow(Client& connection, const TabData& tab)
		{
			if (!connection.browser.alive()) { return nullptr; }
			if (const auto mapped = connection.windows.find(tab.windowId); mapped != connection.windows.end())
			{
				DWORD pid = 0; GetWindowThreadProcessId(mapped->second, &pid);
				if (pid == connection.browser.pid && normalWindow(mapped->second)) { return mapped->second; }
				connection.windows.erase(mapped);
			}
			// A restart loses HWND mappings. Recover only a unique title in the
			// verified browser process; never guess between profiles or windows.
			for (const auto& peer : clients) { for (const auto& [id, other] : peer->tabs) {
				if ((peer->id != connection.id || other.windowId != tab.windowId) && other.title == tab.title) { return nullptr; }
			} }
			struct Search { DWORD pid; std::wstring_view title; HWND found = nullptr; bool ambiguous = false; } search{connection.browser.pid, tab.title};
			winrt::check_bool(EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
				auto& state = *reinterpret_cast<Search*>(parameter);
				DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
				if (pid != state.pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) { return TRUE; }
				wchar_t caption[2048]{}; GetWindowTextW(hwnd, caption, static_cast<int>(std::size(caption)));
				if (chromeTitleMatches(caption, state.title))
				{
					if (state.found) { state.ambiguous = true; } else { state.found = hwnd; }
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&search)));
			if (search.ambiguous || !search.found || !normalWindow(search.found)) { return nullptr; }
			connection.windows[tab.windowId] = search.found;
			log("recovered Chrome window client=" + std::to_string(connection.id) + " window=" + std::to_string(tab.windowId));
			return search.found;
		}
		void learnWindow(Client& connection)
		{
			const auto active = connection.tabs.find(connection.active);
			if (!connection.focused || active == connection.tabs.end() || !connection.browser.alive()) { return; }
			const HWND hwnd = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
			if (pid == connection.browser.pid && normalWindow(hwnd) && chromeTitleMatches(title(hwnd), active->second.title))
			{
				connection.windows[active->second.windowId] = hwnd;
			}
		}
		void foreground(HWND hwnd)
		{
			RECT bounds{}; MONITORINFO monitor{sizeof(monitor)};
			fullscreen = hwnd != overlay && hwnd != GetShellWindow() && hwnd != GetDesktopWindow()
				&& GetWindowRect(hwnd, &bounds) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)
				&& bounds.left <= monitor.rcMonitor.left && bounds.top <= monitor.rcMonitor.top
				&& bounds.right >= monitor.rcMonitor.right && bounds.bottom >= monitor.rcMonitor.bottom;
			if (!normalWindow(hwnd)) { return; }
			recent.visit({0, reinterpret_cast<std::uint64_t>(hwnd)}, now());
			for (auto& connection : clients)
			{
				if (!connection->focused || !connection->tabs.contains(connection->active)) { continue; }
				const int windowId = connection->tabs.at(connection->active).windowId;
				const auto mapped = connection->windows.find(windowId);
				if (mapped != connection->windows.end() && mapped->second == hwnd) { recent.visit({connection->id, static_cast<std::uint64_t>(connection->active)}, now()); }
			}
		}
		static JsonObject messageFor(const wchar_t* type) { return bridge::message(type); }
		void receive(Client& connection, const JsonObject& message)
		{
			if (integer(message, L"version") != 1) { throw winrt::hresult_invalid_argument(L"Protocol version"); }
			const auto type = message.GetNamedString(L"type");
			if (type == L"snapshot")
			{
				const auto snapshot = parseSnapshot(message);
				const bool focused = message.GetNamedBoolean(L"focused", false);
				const bool visited = focused && (!connection.focused || snapshot.activeId != connection.active);
				std::map<int, TabData> next;
				const auto raw = message.GetNamedArray(L"tabs");
				for (size_t index = 0; index < snapshot.tabs.size(); ++index)
				{
					const auto& tab = snapshot.tabs[index]; next.emplace(tab.id, tab);
					const double timestamp = raw.GetAt(static_cast<uint32_t>(index)).GetObject().GetNamedNumber(L"lastAccessed", 0);
					if (!std::isfinite(timestamp) || timestamp < 0 || timestamp > static_cast<double>(now() + 60000)) { throw winrt::hresult_invalid_argument(L"Invalid lastAccessed"); }
					recent.seed({connection.id, static_cast<std::uint64_t>(tab.id)}, static_cast<std::uint64_t>(timestamp));
				}
				std::erase_if(connection.previews, [&](const auto& entry) { return !next.contains(entry.first); });
				std::erase_if(recent.times, [&](const auto& entry) { return entry.first.client == connection.id && !next.contains(static_cast<int>(entry.first.id)); });
				connection.tabs = std::move(next);
				connection.active = snapshot.activeId; connection.focused = focused; connection.hasSnapshot = message.HasKey(L"focused");
				std::erase_if(connection.windows, [&](const auto& entry) {
					return !IsWindow(entry.second) || std::none_of(snapshot.tabs.begin(), snapshot.tabs.end(), [&](const auto& tab) { return tab.windowId == entry.first; });
				});
				learnWindow(connection);
				if (visited && snapshot.activeId >= 0) { recent.visit({connection.id, static_cast<std::uint64_t>(snapshot.activeId)}, now()); }
				auto reply = messageFor(L"synced");
				reply.SetNamedValue(L"count", JsonValue::CreateNumberValue(static_cast<double>(connection.tabs.size())));
				reply.SetNamedValue(L"activeTabId", JsonValue::CreateNumberValue(connection.active)); connection.send(reply);
				if (!connection.hasSnapshot) { auto error = messageFor(L"error"); error.SetNamedValue(L"message", JsonValue::CreateStringValue(L"Reload the extension to enable the new switcher.")); connection.send(error); }
				log("snapshot client=" + std::to_string(connection.id) + " count=" + std::to_string(connection.tabs.size()) + " focused=" + std::to_string(focused) + " active=" + std::to_string(connection.active) + " mapped=" + std::to_string(connection.windows.size()));
			}
			else if (type == L"preview_state")
			{
				connection.previewsEnabled = message.GetNamedBoolean(L"enabled");
				if (!*connection.previewsEnabled) { connection.previews.clear(); connection.previewOrder.clear(); }
				if (showing) { invalidatePreviews(connection.id); }
			}
			else if (type == L"preview")
			{
				const int id = integer(message, L"tabId");
				if (!connection.tabs.contains(id)) { throw winrt::hresult_invalid_argument(L"Preview for absent tab"); }
				auto pixels = decodePreview(message.GetNamedString(L"image"));
				connection.previews.insert_or_assign(id, std::move(pixels));
				std::erase(connection.previewOrder, id); connection.previewOrder.push_back(id);
				while (connection.previewOrder.size() > 48) { connection.previews.erase(connection.previewOrder.front()); connection.previewOrder.pop_front(); }
				auto reply = messageFor(L"previewed"); reply.SetNamedValue(L"tabId", JsonValue::CreateNumberValue(id)); connection.send(reply);
				if (showing) { invalidatePreviews(connection.id, id); }
			}
			else if (type == L"clear_previews")
			{
				if (message.HasKey(L"tabId")) { const int id = integer(message, L"tabId"); connection.previews.erase(id); std::erase(connection.previewOrder, id); }
				else { connection.previews.clear(); connection.previewOrder.clear(); }
				if (showing) { invalidatePreviews(connection.id, message.HasKey(L"tabId") ? integer(message, L"tabId") : -1); }
			}
			else if (test && type == L"test_inspect")
			{
				auto reply = messageFor(L"test_state");
				JsonArray values;
				for (const auto& peer : clients) { for (const auto& [id, tab] : peer->tabs) {
					JsonObject value; value.SetNamedValue(L"client", JsonValue::CreateNumberValue(static_cast<double>(peer->id)));
					value.SetNamedValue(L"id", JsonValue::CreateNumberValue(id)); value.SetNamedValue(L"title", JsonValue::CreateStringValue(tab.title));
					value.SetNamedValue(L"recent", JsonValue::CreateNumberValue(static_cast<double>(recent.get({peer->id, static_cast<std::uint64_t>(id)}))));
					value.SetNamedValue(L"preview", JsonValue::CreateBooleanValue(peer->previews.contains(id)));
					value.SetNamedValue(L"previewsEnabled", peer->previewsEnabled ? JsonValue::CreateBooleanValue(*peer->previewsEnabled) : JsonValue::CreateNullValue()); values.Append(value);
				} }
				reply.SetNamedValue(L"tabs", values); reply.SetNamedValue(L"visible", JsonValue::CreateBooleanValue(IsWindowVisible(overlay) != FALSE));
				reply.SetNamedValue(L"hiddenOwner", JsonValue::CreateBooleanValue(GetWindow(overlay, GW_OWNER) == frameOwner
					&& frameOwner && !IsWindowVisible(frameOwner)));
				reply.SetNamedValue(L"specialWindowStyle", JsonValue::CreateBooleanValue((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & (WS_EX_APPWINDOW | WS_EX_TOOLWINDOW)) != 0));
				BOOL frameRendering = FALSE;
				winrt::check_hresult(DwmGetWindowAttribute(overlay, DWMWA_NCRENDERING_ENABLED, &frameRendering, sizeof(frameRendering)));
				RECT windowBounds{}, clientBounds{}; winrt::check_bool(GetWindowRect(overlay, &windowBounds)); winrt::check_bool(GetClientRect(overlay, &clientBounds));
				reply.SetNamedValue(L"effectsEnabled", JsonValue::CreateBooleanValue(glass));
				reply.SetNamedValue(L"backgroundPixel", JsonValue::CreateNumberValue(frame.pixels ? static_cast<const DWORD*>(frame.pixels)[0] : 0));
				reply.SetNamedValue(L"composedFrame", JsonValue::CreateBooleanValue(glass && static_cast<bool>(contentSurface)));
				reply.SetNamedValue(L"surfaceAllocations", JsonValue::CreateNumberValue(static_cast<double>(surfaceAllocations)));
				reply.SetNamedValue(L"dwmFrame", JsonValue::CreateBooleanValue(frameRendering != FALSE));
				reply.SetNamedValue(L"borderless", JsonValue::CreateBooleanValue(windowBounds.right - windowBounds.left == clientBounds.right
					&& windowBounds.bottom - windowBounds.top == clientBounds.bottom));
				reply.SetNamedValue(L"thumbnailBuilds", JsonValue::CreateNumberValue(static_cast<double>(thumbnailBuilds)));
				reply.SetNamedValue(L"preparedFrames", JsonValue::CreateNumberValue(static_cast<double>(preparedFrames)));
				reply.SetNamedValue(L"preparedWhileHidden", JsonValue::CreateBooleanValue(preparedWhileHidden));
				reply.SetNamedValue(L"frameAllocations", JsonValue::CreateNumberValue(static_cast<double>(frame.allocations)));
				reply.SetNamedValue(L"selected", JsonValue::CreateNumberValue(static_cast<double>(selected)));
				connection.send(reply);
			}
			else if (test && type == L"test_cycle") { show(integer(message, L"direction", -1)); UpdateWindow(overlay); }
			else if (test && type == L"test_cancel") { hide(false); }
			else if (test && type == L"test_close")
			{
				const int peerId = integer(message, L"client"), tabId = integer(message, L"tabId");
				show(1);
				const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == Key{static_cast<std::uint64_t>(peerId), static_cast<std::uint64_t>(tabId)}; });
				if (found == items.end()) { hide(false); throw winrt::hresult_invalid_argument(L"Test close target missing"); }
				closeItem(static_cast<size_t>(found - items.begin()));
			}
			else if (test && type == L"test_select")
			{
				const int peerId = integer(message, L"client"), tabId = integer(message, L"tabId");
				show(1);
				const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == Key{static_cast<std::uint64_t>(peerId), static_cast<std::uint64_t>(tabId)}; });
				if (found == items.end()) { hide(false); throw winrt::hresult_invalid_argument(L"Test selection missing"); }
				const auto old = selected; selected = static_cast<size_t>(found - items.begin()); selectionChanged(old); UpdateWindow(overlay); commitDue = GetTickCount64() + 30;
			}
			else if (type == L"result")
			{
				const bool ok = message.GetNamedBoolean(L"ok");
				log("activation result client=" + std::to_string(connection.id) + " ok=" + std::to_string(ok));
				if (!connection.pending.empty())
				{
					const auto pending = connection.pending.front(); connection.pending.pop_front();
					const auto tab = connection.tabs.find(pending.tab);
					if (ok && pending.sequence == selectionSequence && GetTickCount64() - pending.created < 2000 && tab != connection.tabs.end())
					{
						const HWND target = resolveWindow(connection, tab->second);
						if (target)
						{
							const HWND current = GetForegroundWindow();
							DWORD pid = 0; GetWindowThreadProcessId(target, &pid);
							// Chrome's focused:true result can arrive before Windows brings
							// its window forward. Finish only the latest user-requested
							// handoff, and do not steal focus from an intervening app.
							if (pid == connection.browser.pid && (current == pending.source || current == overlay || current == target))
							{
								if (IsIconic(target)) { ShowWindowAsync(target, SW_RESTORE); }
								log("Chrome foreground handoff=" + std::to_string(focusWindow(target)));
							}
						}
						else { log("Chrome foreground handoff unavailable: window missing or ambiguous"); }
					}
				}
			}
			else { throw winrt::hresult_invalid_argument(L"Unknown protocol message"); }
		}
		void poll(Client& connection)
		{
			for (int budget = 0; budget < 4; ++budget)
			{
				DWORD count = 0;
				if (connection.read.pending)
				{
					if (!GetOverlappedResult(connection.pipe.get(), &connection.read.overlapped, &count, FALSE))
					{
						if (GetLastError() == ERROR_IO_INCOMPLETE) { break; } winrt::throw_last_error();
					}
					connection.read.pending = false;
				}
				else
				{
					connection.read.reset();
					if (!ReadFile(connection.pipe.get(), connection.chunk.data(), static_cast<DWORD>(connection.chunk.size()), &count, &connection.read.overlapped))
					{
						if (GetLastError() != ERROR_IO_PENDING) { winrt::throw_last_error(); } connection.read.pending = true; break;
					}
				}
				if (!count) { throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)); }
				if (connection.input.empty()) { connection.frameStarted = GetTickCount64(); }
				connection.input.insert(connection.input.end(), connection.chunk.data(), connection.chunk.data() + count);
				while (connection.input.size() >= sizeof(std::uint32_t))
				{
					std::uint32_t length = 0; memcpy(&length, connection.input.data(), sizeof(length));
					if (!length || length > maxFrame) { throw winrt::hresult_invalid_argument(L"Invalid frame size"); }
					if (connection.input.size() < sizeof(length) + length) { break; }
					const auto json = winrt::to_hstring(std::string_view(connection.input.data() + sizeof(length), length));
					receive(connection, JsonObject::Parse(json));
					connection.input.erase(connection.input.begin(), connection.input.begin() + sizeof(length) + length);
					connection.frameStarted = GetTickCount64();
				}
			}
			if (!connection.input.empty() && GetTickCount64() - connection.frameStarted > 10000) { throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
			connection.flush();
		}
		void tick()
		{
			heartbeat = GetTickCount64();
			if (quitEvent && WaitForSingleObject(quitEvent.get(), 0) == WAIT_OBJECT_0) { requestQuit(); }
			if (pauseEvent && WaitForSingleObject(pauseEvent.get(), 0) == WAIT_OBJECT_0) { setEnabled(false); }
			if (resumeEvent && WaitForSingleObject(resumeEvent.get(), 0) == WAIT_OBJECT_0 && !quitDue) { setEnabled(true); }
			if (quitDue && GetTickCount64() >= quitDue) { PostQuitMessage(0); return; }
			if (commitDue && GetTickCount64() >= commitDue) { commitDue = 0; hide(true); }
			if (!listener && clients.size() < 8) { listener = std::make_unique<Client>(nextClient++, endpoint); }
			if (listener)
			{
				DWORD ignored = 0;
				if (!listener->read.pending || GetOverlappedResult(listener->pipe.get(), &listener->read.overlapped, &ignored, FALSE))
				{
					listener->read.pending = false;
					try
					{
						verifyPeer(listener->pipe.get(), true);
						ULONG pid = 0; winrt::check_bool(GetNamedPipeClientProcessId(listener->pipe.get(), &pid)); if (!test) { listener->browser.discover(pid); }
						auto ready = messageFor(L"ready"); ready.SetNamedValue(L"hostVersion", JsonValue::CreateStringValue(winrt::to_hstring(CHROME_ALT_TAB_VERSION)));
						ready.SetNamedValue(L"previewState", JsonValue::CreateBooleanValue(true));
						if (listener->browser.alive())
						{
							if (ExtractIconExW(executable(listener->browser.pid).c_str(), 0, &listener->appIcon, nullptr, 1) != 1) { log("Chrome icon unavailable"); }
						}
						ready.SetNamedValue(L"ownerMode", JsonValue::CreateStringValue(L"custom-switcher")); listener->send(ready);
						log("connected client=" + std::to_string(listener->id) + " chromePid=" + std::to_string(listener->browser.pid)); clients.push_back(std::move(listener));
					}
					catch (...) { log("client handshake rejected: " + winrt::to_string(winrt::to_message())); listener.reset(); }
				}
				else if (GetLastError() != ERROR_IO_INCOMPLETE) { listener.reset(); }
			}
			for (auto it = clients.begin(); it != clients.end();)
			{
				try { poll(**it); ++it; }
				catch (...)
				{
					log("disconnected client=" + std::to_string((*it)->id) + ": " + winrt::to_string(winrt::to_message()));
					recent.eraseClient((*it)->id); it = clients.erase(it); if (showing) { hide(false); }
				}
			}
			available = demo || std::any_of(clients.begin(), clients.end(), [](const auto& connection) { return connection->hasSnapshot; });
			if (!clients.empty()) { emptySince = GetTickCount64(); }
			updateTray();
			if (test && !demo && GetTickCount64() - emptySince > 15000) { PostQuitMessage(0); }
		}
		void collect()
		{
			items.clear();
			EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
				auto& owner = *reinterpret_cast<App*>(parameter);
				if (owner.normalWindow(hwnd)) { owner.items.push_back({{0, reinterpret_cast<std::uint64_t>(hwnd)}, owner.title(hwnd), hwnd}); }
				return TRUE;
			}, reinterpret_cast<LPARAM>(this));
			std::erase_if(recent.times, [](const auto& entry) { return !entry.first.client && !IsWindow(reinterpret_cast<HWND>(entry.first.id)); });
			for (auto& connection : clients)
			{
				for (const auto& [id, tab] : connection->tabs)
				{
					const auto mapped = connection->windows.find(tab.windowId); HWND hwnd = mapped == connection->windows.end() ? nullptr : mapped->second;
					if (hwnd && !normalWindow(hwnd)) { continue; }
					items.push_back({{connection->id, static_cast<std::uint64_t>(id)}, std::wstring(tab.title), hwnd});
				}
				// Keep every Chrome window until positively mapped to mirrored tabs.
				std::erase_if(items, [&](const auto& item) {
					return !item.key.client && std::any_of(connection->windows.begin(), connection->windows.end(), [&](const auto& mapped) { return mapped.second == item.hwnd; });
				});
			}
			if (demo) { for (int id = 1; id <= 3; ++id) { items.push_back({{UINT64_MAX, static_cast<std::uint64_t>(id)}, L"Demo tab " + std::to_wstring(id), nullptr}); } }
			std::stable_sort(items.begin(), items.end(), [&](const auto& left, const auto& right) { return recent.get(left.key) > recent.get(right.key); });
			const auto active = std::find_if(items.begin(), items.end(), [&](const auto& item) {
				if (!item.key.client) { return item.hwnd == previous; }
				const auto connection = client(item.key.client);
				return item.hwnd == previous && connection && connection->focused && connection->active == static_cast<int>(item.key.id);
			});
			if (active != items.end()) { std::rotate(items.begin(), active, active + 1); }
		}
		int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
		RECT card(size_t slot) const { return slot < cardRects.size() ? cardRects[slot] : RECT{}; }
		RECT imageRect(size_t slot) const { auto r = card(slot); r.top += px(34); return r; }
		void arrange()
		{
			std::vector<double> aspects;
			const size_t first = selected / pageSize * pageSize;
			for (size_t i = first; i < std::min(first + pageSize, items.size()); ++i)
			{
				double aspect = 1.6; RECT rect{};
				if (items[i].hwnd && SUCCEEDED(DwmGetWindowAttribute(items[i].hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))
					&& !IsIconic(items[i].hwnd) && rect.bottom > rect.top) { aspect = static_cast<double>(rect.right - rect.left) / (rect.bottom - rect.top); }
				if (const auto peer = client(items[i].key.client))
				{
					const auto found = peer->previews.find(static_cast<int>(items[i].key.id));
					if (found != peer->previews.end()) { aspect = static_cast<double>(found->second.width) / found->second.height; }
				}
				aspects.push_back(aspect);
			}
			const auto layout = layoutCards(aspects, std::min(1420, MulDiv(workArea.right - workArea.left, 96, static_cast<int>(dpi)) - 64),
				MulDiv(workArea.bottom - workArea.top, 96, static_cast<int>(dpi)) - 64);
			width = px(layout.width); height = px(layout.height); cardRects.clear();
			for (const auto& r : layout.cards) { cardRects.push_back({px(r.left), px(r.top), px(r.right), px(r.bottom)}); }
			winrt::check_bool(SetWindowPos(overlay, HWND_TOPMOST, workArea.left + (workArea.right - workArea.left - width) / 2,
				workArea.top + (workArea.bottom - workArea.top - height) / 2, width, height, SWP_NOACTIVATE));
		}
		void appearance()
		{
			dpi = GetDpiForWindow(previous); if (!dpi) { dpi = 96; }
			DWORD light = 0, size = sizeof(light);
			dark = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
				L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS || !light;
			const auto replacement = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
			winrt::check_bool(replacement != nullptr); if (font) { DeleteObject(font); } font = replacement;
			const BOOL useDark = dark; const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
			// Decoration failure does not affect switching. Older Windows builds may reject it.
			if (FAILED(DwmSetWindowAttribute(overlay, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark)))) { log("dark frame unavailable"); }
			if (FAILED(DwmSetWindowAttribute(overlay, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners)))) { log("rounded frame unavailable"); }
			HIGHCONTRASTW contrast{sizeof(contrast)};
			const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
			const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
			winrt::check_hresult(DwmSetWindowAttribute(overlay, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
			SYSTEM_POWER_STATUS power{}; winrt::check_bool(GetSystemPowerStatus(&power));
			const bool batterySaver = power.SystemStatusFlag == 1;
			const bool effectsEnabled = !highContrast && !batterySaver
				&& winrt::Windows::UI::ViewManagement::UISettings().AdvancedEffectsEnabled();
			const BOOL hostBackdrop = effectsEnabled;
			winrt::check_hresult(DwmSetWindowAttribute(overlay, DWMWA_USE_HOSTBACKDROPBRUSH, &hostBackdrop, sizeof(hostBackdrop)));
			glass = effectsEnabled;
			if (!glass)
			{
				if (compositionTarget) { compositionTarget.Root(nullptr); }
				const MARGINS none{}; winrt::check_hresult(DwmExtendFrameIntoClientArea(overlay, &none));
				log(highContrast ? "opaque backdrop: high contrast" : batterySaver ? "opaque backdrop: Battery Saver" : "opaque backdrop: Windows advanced effects disabled");
				return;
			}
			if (!compositor)
			{
				DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
				winrt::check_hresult(CreateDispatcherQueueController(options,
					reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dispatcher))));
				compositor = winrt::Windows::UI::Composition::Compositor();
				winrt::com_ptr<ID3D11Device> d3d;
				winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
					nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
				winrt::com_ptr<ID2D1Factory1> factory;
				winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));
				winrt::com_ptr<ID2D1Device> device;
				winrt::check_hresult(factory->CreateDevice(d3d.as<IDXGIDevice>().get(), device.put()));
				winrt::check_hresult(compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateGraphicsDevice(device.get(),
					reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice))));
				contentVisual = compositor.CreateSpriteVisual();
				winrt::check_hresult(compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
					overlay, FALSE, reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(compositionTarget))));
			}
			const MARGINS margins{-1, -1, -1, -1};
			winrt::check_hresult(DwmExtendFrameIntoClientArea(overlay, &margins));
			if (glass)
			{
				auto visual = compositor.CreateSpriteVisual();
				visual.RelativeSizeAdjustment({1.0f, 1.0f});
				visual.Brush(compositor.CreateHostBackdropBrush());
				if (auto oldRoot = compositionTarget.Root()) { oldRoot.as<winrt::Windows::UI::Composition::ContainerVisual>().Children().RemoveAll(); }
				auto root = compositor.CreateContainerVisual(); root.RelativeSizeAdjustment({1.0f, 1.0f});
				root.Children().InsertAtBottom(visual);
				root.Children().InsertAtTop(contentVisual);
				compositionTarget.Root(root);
			}
			else { compositionTarget.Root(nullptr); }
			log(glass ? "compositor host backdrop active" : "opaque backdrop (high contrast)");
		}
		void updateThumbnails()
		{
			if (thumbnailPage == selected / pageSize) { return; }
			clearThumbnails();
			thumbnailPage = selected / pageSize; ++thumbnailBuilds;
			const size_t first = selected / pageSize * pageSize;
			for (size_t index = first; index < std::min(first + pageSize, items.size()); ++index)
			{
				if (items[index].key.client || !items[index].hwnd) { continue; }
				HTHUMBNAIL thumbnail = nullptr;
				if (FAILED(DwmRegisterThumbnail(overlay, items[index].hwnd, &thumbnail))) { continue; }
				DWM_THUMBNAIL_PROPERTIES properties{};
				properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
				properties.rcDestination = imageRect(index - first); properties.fVisible = TRUE; properties.opacity = 255; properties.fSourceClientAreaOnly = FALSE;
				SIZE source{};
				if (SUCCEEDED(DwmQueryThumbnailSourceSize(thumbnail, &source)) && source.cx > 0 && source.cy > 0)
				{
					auto& r = properties.rcDestination;
					const double scale = std::min(static_cast<double>(r.right - r.left) / source.cx, static_cast<double>(r.bottom - r.top) / source.cy);
					const LONG w = static_cast<LONG>(source.cx * scale), h = static_cast<LONG>(source.cy * scale);
					r.left += (r.right - r.left - w) / 2; r.top += (r.bottom - r.top - h) / 2;
					r.right = r.left + w; r.bottom = r.top + h;
				}
				if (SUCCEEDED(DwmUpdateThumbnailProperties(thumbnail, &properties))) { thumbnails.push_back(thumbnail); liveSlots.insert(index - first); } else { DwmUnregisterThumbnail(thumbnail); }
			}
			InvalidateRect(overlay, nullptr, FALSE);
		}
		void show(int direction)
		{
			if (!showing)
			{
				previous = GetForegroundWindow(); collect();
				if (items.empty()) { capturing = false; return; }
				selected = direction < 0 ? items.size() - 1 : (items.size() > 1 ? 1 : 0);
				MONITORINFO monitor{sizeof(monitor)}; winrt::check_bool(GetMonitorInfoW(MonitorFromWindow(previous, MONITOR_DEFAULTTONEAREST), &monitor));
				workArea = monitor.rcWork; hovered = -1; appearance(); arrange();
				updateThumbnails(); prepareFrame();
				if (FAILED(DwmFlush())) { log("initial composition flush unavailable"); }
				ShowWindow(overlay, SW_SHOW);
				showing = true;
				const bool granted = focusWindow(overlay);
				UpdateWindow(overlay);
				log("opened items=" + std::to_string(items.size()) + " foreground=" + std::to_string(granted)
					+ " previousHwnd=" + std::to_string(reinterpret_cast<std::uint64_t>(previous)));
				for (size_t index = 0; index < std::min<size_t>(4, items.size()); ++index)
				{
					const auto& item = items[index];
					log("order " + std::to_string(index) + " client=" + std::to_string(item.key.client) + " id=" + std::to_string(item.key.id)
						+ " hwnd=" + std::to_string(reinterpret_cast<std::uint64_t>(item.hwnd)) + " recent=" + std::to_string(recent.get(item.key)));
				}
			}
			else { const auto old = selected; selected = cycle(selected, items.size(), direction); selectionChanged(old); }
		}
		void navigate(UINT key)
		{
			if (!showing || items.empty()) { return; }
			const size_t old = selected, oldPage = selected / pageSize;
			if (key == VK_LEFT || key == VK_RIGHT) { selected = cycle(selected, items.size(), key == VK_LEFT ? -1 : 1); }
			else if (key == VK_HOME) { selected = 0; }
			else if (key == VK_END) { selected = items.size() - 1; }
			else
			{
				std::vector<CardRect> cards; for (const auto& r : cardRects) { cards.push_back({r.left, r.top, r.right, r.bottom}); }
				selected = oldPage * pageSize + verticalCard(cards, selected % pageSize, key == VK_UP ? -1 : 1);
			}
			selectionChanged(old);
		}
		void invalidateCard(size_t index)
		{
			if (index >= items.size() || index / pageSize != selected / pageSize) { return; }
			auto dirty = card(index % pageSize); InflateRect(&dirty, px(8), px(8)); InvalidateRect(overlay, &dirty, FALSE);
		}
		void invalidatePreviews(std::uint64_t peer, int tab = -1)
		{
			const size_t first = selected / pageSize * pageSize;
			for (size_t i = first; i < std::min(first + pageSize, items.size()); ++i)
			{
				if (items[i].key.client == peer && (tab < 0 || items[i].key.id == static_cast<std::uint64_t>(tab))) { invalidateCard(i); }
			}
		}
		void selectionChanged(size_t old)
		{
			if (old == selected) { return; }
			if (old / pageSize != selected / pageSize) { hovered = -1; arrange(); updateThumbnails(); }
			else { invalidateCard(old); invalidateCard(selected); }
		}
		RECT closeRect(size_t slot) const { auto r = card(slot); r.left = r.right - px(28); r.bottom = r.top + px(32); return r; }
		void closeItem(size_t index)
		{
			if (index >= items.size()) { return; }
			const auto item = items[index]; hide(false);
			if (auto peer = client(item.key.client))
			{
				if (!peer->tabs.contains(static_cast<int>(item.key.id))) { return; }
				auto command = messageFor(L"close"); command.SetNamedValue(L"tabId", JsonValue::CreateNumberValue(static_cast<double>(item.key.id)));
				peer->send(command); peer->flush();
			}
			else if (item.hwnd && IsWindow(item.hwnd) && !PostMessageW(item.hwnd, WM_CLOSE, 0, 0)) { log("window refused close request"); }
		}
		void hide(bool commit)
		{
			capturing = false;
			if (!showing) { return; }
			showing = false; commitDue = 0; preparedPaint = false;
			if (commit && selected < items.size())
			{
				const auto item = items[selected];
				++selectionSequence;
				if (item.key.client)
				{
					if (auto connection = client(item.key.client); connection && connection->tabs.contains(static_cast<int>(item.key.id)))
					{
						const auto& tab = connection->tabs.at(static_cast<int>(item.key.id));
						HWND target = resolveWindow(*connection, tab);
						// The selected tab may be in the background. Its currently active
						// sibling can identify the same browser window before activation.
						if (!target) { if (const auto active = connection->tabs.find(connection->active);
							active != connection->tabs.end() && active->second.windowId == tab.windowId) { target = resolveWindow(*connection, active->second); } }
						if (target)
						{
							if (IsIconic(target)) { ShowWindowAsync(target, SW_RESTORE); }
							log("Chrome pre-activation foreground=" + std::to_string(focusWindow(target)));
						}
						const bool granted = connection->browser.alive() && AllowSetForegroundWindow(connection->browser.pid);
						auto command = messageFor(L"activate"); command.SetNamedValue(L"tabId", JsonValue::CreateNumberValue(static_cast<double>(item.key.id)));
						if (connection->pending.size() >= 16) { throw winrt::hresult_error(E_FAIL, L"Activation acknowledgements stalled"); }
						connection->pending.push_back({static_cast<int>(item.key.id), previous, GetTickCount64(), selectionSequence});
						connection->send(command); connection->flush();
						log("activate client=" + std::to_string(item.key.client) + " tab=" + std::to_string(item.key.id) + " foregroundGrant=" + std::to_string(granted));
					}
				}
				else if (IsWindow(item.hwnd))
				{
					if (IsIconic(item.hwnd)) { ShowWindowAsync(item.hwnd, SW_RESTORE); }
					log("activate window foreground=" + std::to_string(focusWindow(item.hwnd)));
				}
			}
			else if (GetForegroundWindow() == overlay && IsWindow(previous)) { focusWindow(previous); }
			ShowWindow(overlay, SW_HIDE); clearThumbnails(); items.clear(); log(commit ? "closed after selection" : "closed after cancellation");
		}
		void presentFrame(HDC target, RECT dirty)
		{
			if (!glass)
			{
				winrt::check_bool(BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
					frame.dc, dirty.left, dirty.top, SRCCOPY));
				winrt::check_bool(GdiFlush()); return;
			}
			using namespace winrt::Windows::Graphics::DirectX;
			if (!contentSurface || surfaceWidth != width || surfaceHeight != height)
			{
				contentSurface = graphicsDevice.CreateDrawingSurface({static_cast<float>(width), static_cast<float>(height)},
					DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
				contentVisual.Brush(compositor.CreateSurfaceBrush(contentSurface));
				contentVisual.Size({static_cast<float>(width), static_cast<float>(height)});
				surfaceWidth = width; surfaceHeight = height; contentBitmap = nullptr; ++surfaceAllocations;
				dirty = {0, 0, width, height};
			}
			if (IsRectEmpty(&dirty)) { return; }
			auto surface = contentSurface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
			winrt::com_ptr<ID2D1DeviceContext> dc; POINT offset{};
			winrt::check_hresult(surface->BeginDraw(&dirty, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
			try
			{
				if (!contentBitmap)
				{
					const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
						D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
					winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, contentBitmap.put()));
				}
				const D2D1_RECT_U upload{static_cast<UINT32>(dirty.left), static_cast<UINT32>(dirty.top),
					static_cast<UINT32>(dirty.right), static_cast<UINT32>(dirty.bottom)};
				const auto pixels = static_cast<const DWORD*>(frame.pixels) + dirty.top * width + dirty.left;
				winrt::check_hresult(contentBitmap->CopyFromMemory(&upload, pixels, static_cast<UINT32>(width * sizeof(DWORD))));
				dc->SetDpi(96.0f, 96.0f);
				dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x - dirty.left), static_cast<float>(offset.y - dirty.top)));
				dc->Clear(D2D1::ColorF(0, 0.0f));
				const D2D1_RECT_F rectangle{static_cast<float>(dirty.left), static_cast<float>(dirty.top),
					static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)};
				dc->DrawBitmap(contentBitmap.get(), &rectangle, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &rectangle, nullptr);
			}
			catch (...) { surface->EndDraw(); throw; }
			winrt::check_hresult(surface->EndDraw());
		}
		void prepareFrame()
		{
			const HDC target = GetDC(overlay); winrt::check_bool(target != nullptr);
			try { draw(target, {0, 0, width, height}, false); if (glass) { presentFrame(target, {0, 0, width, height}); } }
			catch (...) { ReleaseDC(overlay, target); throw; }
			ReleaseDC(overlay, target); preparedPaint = true; preparedWhileHidden = !IsWindowVisible(overlay); ++preparedFrames;
		}
		void paint()
		{
			PAINTSTRUCT paint{}; const HDC target = BeginPaint(overlay, &paint);
			try
			{
				if (preparedPaint) { preparedPaint = false; presentFrame(target, {0, 0, width, height}); }
				else { draw(target, paint.rcPaint); }
			}
			catch (...) { EndPaint(overlay, &paint); throw; }
			EndPaint(overlay, &paint);
		}
		void draw(HDC target, RECT dirty, bool present = true)
		{
			frame.resize(target, width, height); const HDC dc = frame.dc;
			const int saved = SaveDC(dc); winrt::check_bool(saved != 0);
			struct Restore { HDC dc; int saved; ~Restore() { RestoreDC(dc, saved); } } restore{dc, saved};
			IntersectClipRect(dc, dirty.left, dirty.top, dirty.right, dirty.bottom);
			RECT bounds{0, 0, width, height};
			const HBRUSH background = CreateSolidBrush(glass ? RGB(0, 0, 0) : (dark ? RGB(48, 48, 48) : RGB(235, 235, 235))); FillRect(dc, &bounds, background); DeleteObject(background);
			const auto oldFont = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, dark ? RGB(255, 255, 255) : RGB(20, 20, 20));
			const size_t first = selected / pageSize * pageSize;
			for (size_t index = first; index < std::min(first + pageSize, items.size()); ++index)
			{
				auto r = card(index - first);
				RECT affected = r, overlap{}; InflateRect(&affected, px(8), px(8));
				if (!IntersectRect(&overlap, &affected, &dirty)) { continue; }
				if (index == selected)
				{
					const auto outline = CreatePen(PS_SOLID, px(3), dark ? RGB(85, 195, 240) : RGB(0, 95, 184));
					const auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH)), oldPen = SelectObject(dc, outline);
					RoundRect(dc, r.left - px(6), r.top - px(6), r.right + px(6), r.bottom + px(6), px(24), px(24));
					SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(outline);
				}
				const auto brush = CreateSolidBrush(dark ? (hovered == static_cast<int>(index) ? RGB(36, 36, 36) : RGB(24, 24, 24)) : RGB(250, 250, 250));
				const auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
				RoundRect(dc, r.left, r.top, r.right, r.bottom, px(16), px(16));
				SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush);
				auto image = imageRect(index - first); const PreviewPixels* preview = nullptr;
				if (auto connection = client(items[index].key.client))
				{
					const auto found = connection->previews.find(static_cast<int>(items[index].key.id)); if (found != connection->previews.end()) { preview = &found->second; }
				}
				if (preview)
				{
					BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = static_cast<LONG>(preview->width);
					info.bmiHeader.biHeight = -static_cast<LONG>(preview->height); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
					const double scale = std::min(static_cast<double>(image.right - image.left) / preview->width, static_cast<double>(image.bottom - image.top) / preview->height);
					const int w = static_cast<int>(preview->width * scale), h = static_cast<int>(preview->height * scale);
					const auto clip = CreateRoundRectRgn(image.left, image.top, image.right + 1, image.bottom + 1, px(10), px(10));
					const int imageState = SaveDC(dc); winrt::check_bool(imageState != 0);
					ExtSelectClipRgn(dc, clip, RGN_AND); DeleteObject(clip);
					SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
					StretchDIBits(dc, image.left + (image.right - image.left - w) / 2, image.top + (image.bottom - image.top - h) / 2, w, h, 0, 0,
						static_cast<int>(preview->width), static_cast<int>(preview->height), preview->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
					RestoreDC(dc, imageState);
				}
				// Class icons are borrowed: no blocking WM_GETICON call to another app.
				HICON icon = items[index].hwnd ? reinterpret_cast<HICON>(GetClassLongPtrW(items[index].hwnd, GCLP_HICON)) : nullptr;
				if (!icon && items[index].hwnd) { icon = reinterpret_cast<HICON>(GetClassLongPtrW(items[index].hwnd, GCLP_HICONSM)); }
				const auto connection = client(items[index].key.client);
				if (!icon && connection) { icon = connection->appIcon; }
				if (!icon) { icon = LoadIconW(nullptr, IDI_APPLICATION); }
				if (!preview && !liveSlots.contains(index - first))
				{
					if (connection && connection->previewsEnabled == false)
					{
						const wchar_t* hint = L"Enable previews\nin the extension";
						RECT text = image; DrawTextW(dc, hint, -1, &text, DT_CENTER | DT_CALCRECT | DT_NOPREFIX);
						image.top += std::max(0L, (image.bottom - image.top - (text.bottom - text.top)) / 2);
						DrawTextW(dc, hint, -1, &image, DT_CENTER | DT_NOPREFIX);
					}
					else
					{
						const int size = std::max(1, std::min({px(64), static_cast<int>(image.right - image.left), static_cast<int>(image.bottom - image.top)}));
						DrawIconEx(dc, image.left + (image.right - image.left - size) / 2, image.top + (image.bottom - image.top - size) / 2,
							icon, size, size, 0, nullptr, DI_NORMAL);
					}
				}
				DrawIconEx(dc, r.left + px(10), r.top + px(8), icon, px(16), px(16), 0, nullptr, DI_NORMAL);
				RECT text{r.left + px(34), r.top + px(6), r.right - px(30), r.top + px(28)};
				DrawTextW(dc, items[index].title.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
				if (index == selected || hovered == static_cast<int>(index))
				{
					const auto close = closeRect(index - first); const int x = (close.left + close.right) / 2, y = (close.top + close.bottom) / 2;
					const auto pen = CreatePen(PS_SOLID, px(1), dark ? RGB(230, 230, 230) : RGB(30, 30, 30)); const auto old = SelectObject(dc, pen);
					MoveToEx(dc, x - px(4), y - px(4), nullptr); LineTo(dc, x + px(4), y + px(4));
					MoveToEx(dc, x + px(4), y - px(4), nullptr); LineTo(dc, x - px(4), y + px(4));
					SelectObject(dc, old); DeleteObject(pen);
				}
			}
			// GDI does not maintain alpha. Keep the extended frame transparent,
			// but make card content (including true black image pixels) opaque.
			if (!GdiFlush()) { throw winrt::hresult_error(E_FAIL, L"GDI frame synchronization failed"); }
			auto pixels = static_cast<DWORD*>(frame.pixels);
			dirty.left = std::max(0L, dirty.left); dirty.top = std::max(0L, dirty.top);
			dirty.right = std::min<LONG>(width, dirty.right); dirty.bottom = std::min<LONG>(height, dirty.bottom);
			for (int y = dirty.top; y < dirty.bottom; ++y) { for (int x = dirty.left; x < dirty.right; ++x) {
				auto& pixel = pixels[y * width + x]; pixel = (pixel & 0x00ffffff) | ((!glass || (pixel & 0x00ffffff)) ? 0xff000000 : 0);
			} }
			if (glass)
			{
				const int radius = px(8);
				for (const auto& r : cardRects)
				{
					for (int y = std::max(dirty.top, r.top); y < std::min(dirty.bottom, r.bottom); ++y)
					{
						for (int x = std::max(dirty.left, r.left); x < std::min(dirty.right, r.right); ++x)
						{
							const int dx = std::max({static_cast<int>(r.left) + radius - x, x - static_cast<int>(r.right) + radius + 1, 0});
							const int dy = std::max({static_cast<int>(r.top) + radius - y, y - static_cast<int>(r.bottom) + radius + 1, 0});
							if (dx * dx + dy * dy <= radius * radius) { pixels[y * width + x] |= 0xff000000; }
						}
					}
				}
			}
			// Theme tint is premultiplied alpha over the compositor's blurred host brush.
			if (glass) { for (int y = dirty.top; y < dirty.bottom; ++y) { for (int x = dirty.left; x < dirty.right; ++x) {
				auto& pixel = pixels[y * width + x]; if (!(pixel & 0xff000000)) { pixel = dark ? 0x33202020 : 0x66666666; }
			} } }
			if (present) { presentFrame(target, dirty); }
			if (!GdiFlush()) { throw winrt::hresult_error(E_FAIL, L"GDI frame synchronization failed"); } SelectObject(dc, oldFont);
		}
	};
	App* app = nullptr;
	LRESULT CALLBACK keyboard(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION)
		{
			const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
			const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
			const bool alt = (key.flags & LLKHF_ALTDOWN) != 0;
			if (down && alt && key.vkCode == VK_F10 && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
			{
				enabled = false; capturing = false; PostMessageW(overlay, keyMessage, 4, 0);
			}
			if (GetTickCount64() - heartbeat.load() > 500) { capturing = false; return CallNextHookEx(nullptr, code, wParam, lParam); }
			// A dedicated hook thread only posts switching commands. No input is
			// recorded, and image decoding cannot block this hook's message loop.
			if (down && alt && key.vkCode == VK_TAB && enabled && available && !fullscreen
				&& !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000))
			{
				capturing = true;
				if (PostMessageW(overlay, keyMessage, (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2 : 1, 0)) { return 1; } capturing = false;
			}
			if (capturing)
			{
				if (key.vkCode == VK_LEFT || key.vkCode == VK_RIGHT || key.vkCode == VK_UP || key.vkCode == VK_DOWN || key.vkCode == VK_HOME || key.vkCode == VK_END)
				{
					if (down) { PostMessageW(overlay, keyMessage, 5, key.vkCode); } return 1;
				}
				if (!down && (key.vkCode == VK_LMENU || key.vkCode == VK_RMENU || key.vkCode == VK_MENU)) { PostMessageW(overlay, keyMessage, 3, 0); capturing = false; }
				if (key.vkCode == VK_TAB) { return 1; }
				if (key.vkCode == VK_ESCAPE || key.vkCode == VK_RETURN)
				{
					if (down) { PostMessageW(overlay, keyMessage, key.vkCode == VK_ESCAPE ? 4 : 3, 0); } return 1;
				}
			}
		}
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}
	void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
	{
		try { if (app) { app->foreground(hwnd); } } catch (...) { log("foreground observation failed"); }
	}
	LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		try
		{
			if (app)
			{
				if (taskbarCreated && message == taskbarCreated) { app->updateTray(true); return 0; }
				switch (message)
				{
				case trayMessage: if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) { app->trayMenu(); } return 0;
				case WM_TIMER: app->tick(); return 0;
				case keyMessage:
					if (wParam == 5) { app->navigate(static_cast<UINT>(lParam)); } else if (wParam == 1 || wParam == 2) { if (enabled && available) { app->show(wParam == 2 ? -1 : 1); } } else if (wParam == 3) { app->commitDue = GetTickCount64() + 30; } else if (wParam == 4 && !enabled) { app->setEnabled(false); } else { app->hide(false); } return 0;
				case WM_PAINT: app->paint(); return 0;
				case WM_ERASEBKGND: return 1;
				// Retain DWM frame integration without a title bar or resize border.
				case WM_NCCALCSIZE: if (wParam) { return 0; } break;
				case WM_NCHITTEST: return HTCLIENT;
				case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) { return 0; } break;
				case WM_HOTKEY: if (wParam == 3) { if (enabled && available) { app->show(1); } return 0; } app->setEnabled(wParam == 2); log(enabled ? "custom Alt+Tab resumed" : "custom Alt+Tab suspended"); return 0;
				case WM_KEYDOWN: if (app->showing && (wParam == VK_ESCAPE || wParam == VK_RETURN)) { app->hide(wParam == VK_RETURN); return 0; } if (app->showing && (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN || wParam == VK_HOME || wParam == VK_END)) { app->navigate(static_cast<UINT>(wParam)); return 0; } break;
				case WM_MOUSEMOVE:
				{
					const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; int hovered = -1;
					for (size_t slot = 0; slot < app->cardRects.size(); ++slot) { const auto r = app->card(slot); if (PtInRect(&r, point)) { hovered = static_cast<int>(app->selected / pageSize * pageSize + slot); break; } }
					if (hovered != app->hovered) { if (app->hovered >= 0) { app->invalidateCard(static_cast<size_t>(app->hovered)); } app->hovered = hovered; if (hovered >= 0) { app->invalidateCard(static_cast<size_t>(hovered)); } }
					TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tracking); return 0;
				}
				case WM_MOUSELEAVE: if (app->hovered >= 0) { app->invalidateCard(static_cast<size_t>(app->hovered)); } app->hovered = -1; return 0;
				case WM_LBUTTONUP:
					for (size_t slot = 0; slot < pageSize; ++slot)
					{
						const auto r = app->card(slot); const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; const size_t index = app->selected / pageSize * pageSize + slot;
						if (PtInRect(&r, point) && index < app->items.size()) { const auto close = app->closeRect(slot); if (PtInRect(&close, point)) { app->closeItem(index); } else { app->selected = index; app->hide(true); } break; }
					} return 0;
				// Preserve default activation and keyboard focus bookkeeping.
				case WM_ACTIVATE: if (LOWORD(wParam) == WA_INACTIVE && app->showing) { app->hide(false); } break;
				case WM_CLOSE: app->hide(false); if (app->demo) { PostQuitMessage(0); } return 0;
				}
			}
		}
		catch (...) { log("switcher error: " + winrt::to_string(winrt::to_message())); enabled = false; capturing = false; ShowWindow(hwnd, SW_HIDE); PostQuitMessage(1); }
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}
}
int runSwitcher(bool demo, bool test)
{
	App state(demo, test);
	winrt::handle singleton{CreateMutexW(&state.endpoint.security, FALSE, state.endpoint.mutex().c_str())}; winrt::check_bool(static_cast<bool>(singleton));
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		winrt::handle resume{OpenEventW(EVENT_MODIFY_STATE, FALSE, (state.endpoint.mutex() + L"-resume").c_str())};
		if (resume) { winrt::check_bool(SetEvent(resume.get())); }
		return 0;
	}
	diagnostics.open(std::filesystem::path(executable()).parent_path() / "chrome-alt-tab-switcher.log", std::ios::trunc); log("custom switcher " CHROME_ALT_TAB_VERSION " starting");
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	app = &state;
	WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = windowProc; type.lpszClassName = L"ChromeAltTabCustomSwitcher"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	type.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(IDI_CHROME_ALT_TAB)); winrt::check_bool(type.hIcon != nullptr);
	winrt::check_bool(RegisterClassW(&type));
	// A permanently hidden owner excludes the popup from Alt+Tab and the taskbar.
	// The compositor supplies the background without another visible app window.
	state.frameOwner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP | WS_CAPTION | WS_THICKFRAME,
		0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
	winrt::check_bool(state.frameOwner != nullptr);
	// Retain a DWM-rendered frame; WM_NCCALCSIZE removes its visible chrome.
	// Visual tests deliberately use the same ownership and styles as production.
	overlay = CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"Windows and Chrome tabs", WS_POPUP | WS_CAPTION | WS_THICKFRAME,
		0, 0, 1120, 510, state.frameOwner, nullptr, type.hInstance, nullptr);
	winrt::check_bool(overlay != nullptr);
	const BOOL noTransition = TRUE;
	if (FAILED(DwmSetWindowAttribute(overlay, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransition, sizeof(noTransition)))) { log("popup transition suppression unavailable"); }
	state.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"); winrt::check_bool(state.font != nullptr);
	CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(state.desktops.put()));
	state.foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS); winrt::check_bool(state.foregroundHook != nullptr);
	if (!test)
	{
		winrt::check_bool(RegisterHotKey(overlay, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10));
		winrt::check_bool(RegisterHotKey(overlay, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9));
		if (!RegisterHotKey(overlay, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8)) { log("diagnostic shortcut unavailable"); }
	}
	if (!test && !demo)
	{
		taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated"); winrt::check_bool(taskbarCreated != 0);
		state.quitEvent.attach(CreateEventW(&state.endpoint.security, FALSE, FALSE, (state.endpoint.mutex() + L"-quit").c_str()));
		state.pauseEvent.attach(CreateEventW(&state.endpoint.security, FALSE, FALSE, (state.endpoint.mutex() + L"-pause").c_str()));
		state.resumeEvent.attach(CreateEventW(&state.endpoint.security, FALSE, FALSE, (state.endpoint.mutex() + L"-resume").c_str()));
		winrt::check_bool(state.quitEvent && state.pauseEvent && state.resumeEvent);
		state.updateTray();
	}
	state.startKeyboard();
	winrt::check_bool(SetTimer(overlay, pollTimer, 20, nullptr) != 0);
	state.foreground(GetForegroundWindow());
	if (demo && test) { state.show(1); }
	MSG message{}; BOOL result = 0;
	while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
	enabled = false; capturing = false;
	if (state.keyboardThread.joinable()) { PostThreadMessageW(state.keyboardThreadId, WM_QUIT, 0, 0); state.keyboardThread.join(); }
	state.hide(false); KillTimer(overlay, pollTimer); UnregisterHotKey(overlay, 1); UnregisterHotKey(overlay, 2); UnregisterHotKey(overlay, 3); DestroyWindow(overlay); app = nullptr;
	if (result == -1) { winrt::throw_last_error(); }
	log("custom switcher stopped"); return static_cast<int>(message.wParam);
}
