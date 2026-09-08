#include "switcher_ipc.h"
#include <thread>
#include <cstdio>

int runSwitcher(bool demo, bool test);
namespace
{
	DWORD transfer(HANDLE pipe, void* buffer, DWORD size, bool writing, HANDLE stopped)
	{
		winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
		winrt::check_bool(static_cast<bool>(event));
		OVERLAPPED operation{}; operation.hEvent = event.get();
		DWORD count = 0;
		const BOOL complete = writing ? WriteFile(pipe, buffer, size, &count, &operation) : ReadFile(pipe, buffer, size, &count, &operation);
		if (!complete)
		{
			if (GetLastError() != ERROR_IO_PENDING) { winrt::throw_last_error(); }
			const HANDLE events[]{event.get(), stopped};
			const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
			if (result != WAIT_OBJECT_0) { CancelIoEx(pipe, &operation); }
			winrt::check_bool(GetOverlappedResult(pipe, &operation, &count, TRUE));
			if (result != WAIT_OBJECT_0) { throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)); }
		}
		return count;
	}
	void upstreamPump(HANDLE pipe, HANDLE stopped)
	{
		char buffer[65536]; DWORD count = 0;
		while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count)
		{
			DWORD offset = 0;
			while (offset < count) { const auto sent = transfer(pipe, buffer + offset, count - offset, true, stopped); if (!sent) { return; } offset += sent; }
		}
	}
	void downstreamPump(HANDLE pipe, HANDLE stopped)
	{
		char buffer[65536];
		for (;;)
		{
			const auto count = transfer(pipe, buffer, sizeof(buffer), false, stopped);
			if (!count) { return; }
			switcher::writeAll(GetStdHandle(STD_OUTPUT_HANDLE), buffer, count);
		}
	}
	int bridgeMain()
	{
		switcher::Endpoint endpoint;
		winrt::handle pipe;
		bool launched = false;
		const auto deadline = GetTickCount64() + 8000;
		while (GetTickCount64() < deadline)
		{
			pipe.attach(CreateFileW(endpoint.pipe().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
			if (pipe.get() != INVALID_HANDLE_VALUE) { break; }
			pipe.detach();
			const auto error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) { winrt::throw_last_error(); }
			if (!launched)
			{
				auto command = L"\"" + switcher::executable() + L"\" --broker";
				STARTUPINFOW startup{sizeof(startup)};
				PROCESS_INFORMATION process{};
				winrt::check_bool(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
				launched = true;
			}
			Sleep(40);
		}
		if (!pipe) { throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
		switcher::verifyPeer(pipe.get(), false);
		winrt::handle finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
		winrt::check_bool(static_cast<bool>(finished));
		std::thread upstream([&] {
			try { upstreamPump(pipe.get(), finished.get()); } catch (...) {}
			SetEvent(finished.get());
		});
		std::thread downstream;
		try
		{
			downstream = std::thread([&] {
				try { downstreamPump(pipe.get(), finished.get()); } catch (...) {}
				SetEvent(finished.get());
			});
		}
		catch (...)
		{
			while (WaitForSingleObject(upstream.native_handle(), 0) == WAIT_TIMEOUT) { CancelSynchronousIo(upstream.native_handle()); Sleep(10); }
			upstream.join();
			throw;
		}
		WaitForSingleObject(finished.get(), INFINITE);
		CancelIoEx(pipe.get(), nullptr);
		// Either EOF ends both directions, including a blocked Chrome stdin read.
		for (auto* thread : {&upstream, &downstream})
		{
			while (WaitForSingleObject(thread->native_handle(), 0) == WAIT_TIMEOUT)
			{
				CancelSynchronousIo(thread->native_handle());
				Sleep(10);
			}
			thread->join();
		}
		return 0;
	}
}
int wmain(int argc, wchar_t** argv)
{
	try
	{
		winrt::init_apartment(winrt::apartment_type::single_threaded);
		if (argc == 2 && std::wstring_view(argv[1]) == L"--broker") { return runSwitcher(false, false); }
		if (argc == 2 && std::wstring_view(argv[1]) == L"--demo") { return runSwitcher(true, false); }
		if (argc == 2 && std::wstring_view(argv[1]) == L"--visual-test") { return runSwitcher(true, true); }
		if (argc == 2 && std::wstring_view(argv[1]) == L"--test-broker") { return runSwitcher(false, true); }
		if (argc == 2 && (std::wstring_view(argv[1]) == L"--quit" || std::wstring_view(argv[1]) == L"--pause" || std::wstring_view(argv[1]) == L"--resume"))
		{
			switcher::Endpoint endpoint;
			const auto suffix = std::wstring(argv[1] + 1);
			winrt::handle command{OpenEventW(EVENT_MODIFY_STATE, FALSE, (endpoint.mutex() + suffix).c_str())};
			if (command) { winrt::check_bool(SetEvent(command.get())); return 0; }
			if (GetLastError() != ERROR_FILE_NOT_FOUND) { winrt::throw_last_error(); }
			return suffix == L"-resume" ? runSwitcher(false, false) : 0;
		}
		if (argc < 2) { return runSwitcher(false, false); }
		const std::wstring_view origin(argv[1]);
		constexpr std::wstring_view prefix = L"chrome-extension://";
		if (!origin.starts_with(prefix) || origin.size() != prefix.size() + 33 || origin.back() != L'/'
			|| origin.substr(prefix.size(), 32).find_first_not_of(L"abcdefghijklmnop") != std::wstring_view::npos)
		{
			throw winrt::hresult_invalid_argument(L"Invalid native messaging origin");
		}
		return bridgeMain();
	}
	catch (...) { fwprintf(stderr, L"Chrome Alt+Tab: %s\n", winrt::to_message().c_str()); return 1; }
}
