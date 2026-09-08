#pragma once
#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winrt/base.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdint>
#include "protocol.h"

namespace switcher
{
	inline std::wstring executable(DWORD pid = GetCurrentProcessId())
	{
		winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
		winrt::check_bool(static_cast<bool>(process));
		std::wstring path(32768, L'\0');
		DWORD size = static_cast<DWORD>(path.size());
		winrt::check_bool(QueryFullProcessImageNameW(process.get(), 0, path.data(), &size));
		path.resize(size);
		return path;
	}
	struct Endpoint
	{
		std::wstring name;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
		Endpoint()
		{
			winrt::handle token;
			winrt::check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));
			DWORD size = 0;
			GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) { winrt::throw_last_error(); }
			std::vector<BYTE> data(size);
			winrt::check_bool(GetTokenInformation(token.get(), TokenUser, data.data(), size, &size));
			LPWSTR sid = nullptr;
			winrt::check_bool(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid));
			const std::wstring user(sid);
			LocalFree(sid);
			DWORD session = 0;
			winrt::check_bool(ProcessIdToSessionId(GetCurrentProcessId(), &session));
			std::uint64_t hash = 14695981039346656037ull;
			for (wchar_t ch : executable()) { hash = (hash ^ static_cast<unsigned short>(towlower(ch))) * 1099511628211ull; }
			name = L"chrome-alt-tab-v4-" + user + L"-" + std::to_wstring(session) + L"-" + std::to_wstring(hash);
			const auto sddl = L"D:P(A;;GA;;;" + user + L")";
			winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
			security.lpSecurityDescriptor = descriptor;
		}
		~Endpoint() { if (descriptor) { LocalFree(descriptor); } }
		Endpoint(const Endpoint&) = delete;
		Endpoint& operator=(const Endpoint&) = delete;
		std::wstring pipe() const { return L"\\\\.\\pipe\\" + name; }
		std::wstring mutex() const { return L"Local\\" + name; }
	};
	inline void verifyPeer(HANDLE pipe, bool server)
	{
		ULONG pid = 0;
		winrt::check_bool(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid));
		if (_wcsicmp(executable(pid).c_str(), executable().c_str()) != 0)
		{
			throw winrt::hresult_access_denied(L"IPC peer is not this switcher executable");
		}
	}
	inline void writeAll(HANDLE output, const void* data, DWORD size)
	{
		auto bytes = static_cast<const char*>(data);
		while (size)
		{
			DWORD count = 0;
			winrt::check_bool(WriteFile(output, bytes, size, &count, nullptr));
			if (!count) { throw winrt::hresult_error(E_FAIL); }
			bytes += count;
			size -= count;
		}
	}
	inline std::vector<char> frame(const bridge::JsonObject& message)
	{
		const auto json = winrt::to_string(message.Stringify());
		if (json.empty() || json.size() > bridge::maxFrame) { throw winrt::hresult_invalid_argument(); }
		const auto length = static_cast<std::uint32_t>(json.size());
		std::vector<char> result(sizeof(length) + json.size());
		memcpy(result.data(), &length, sizeof(length));
		memcpy(result.data() + sizeof(length), json.data(), json.size());
		return result;
	}
	// Chrome can launch a native host through cmd.exe. Validate each ancestor's
	// creation time, and retain Chrome's handle to prevent PID reuse.
	struct BrowserProcess
	{
		winrt::handle process;
		DWORD pid = 0;
		void discover(DWORD child)
		{
			winrt::handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
			winrt::check_bool(snapshot.get() != INVALID_HANDLE_VALUE);
			std::vector<PROCESSENTRY32W> entries;
			PROCESSENTRY32W entry{sizeof(entry)};
			winrt::check_bool(Process32FirstW(snapshot.get(), &entry));
			do { entries.push_back(entry); } while (Process32NextW(snapshot.get(), &entry));
			FILETIME youngest{}, exit{}, kernel{}, user{};
			winrt::handle current{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child)};
			if (!current || !GetProcessTimes(current.get(), &youngest, &exit, &kernel, &user)) { return; }
			for (int depth = 0; depth < 8; ++depth)
			{
				const auto found = std::find_if(entries.begin(), entries.end(), [child](const auto& item) { return item.th32ProcessID == child; });
				if (found == entries.end() || found->th32ParentProcessID == child) { return; }
				child = found->th32ParentProcessID;
				winrt::handle candidate{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, child)};
				FILETIME created{};
				if (!candidate || !GetProcessTimes(candidate.get(), &created, &exit, &kernel, &user)
					|| CompareFileTime(&created, &youngest) > 0) { return; }
				youngest = created;
				if (_wcsicmp(std::filesystem::path(executable(child)).filename().c_str(), L"chrome.exe") == 0)
				{
					pid = child;
					process = std::move(candidate);
					return;
				}
			}
		}
		bool alive() const { return process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT; }
	};
}
