#pragma once
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace bridge
{
	using namespace winrt::Windows::Data::Json;
	constexpr std::uint32_t maxFrame = 1024 * 1024;
	struct TabData
	{
		int id;
		int windowId;
		winrt::hstring title;
	};
	struct Snapshot
	{
		std::vector<TabData> tabs;
		int activeId = -1;
	};
	inline int integer(const JsonObject& object, const wchar_t* key, int minimum = 0)
	{
		const double value = object.GetNamedNumber(key);
		if (!std::isfinite(value) || std::floor(value) != value || value < minimum || value > INT32_MAX)
		{
			throw winrt::hresult_invalid_argument(L"Invalid integer field");
		}
		return static_cast<int>(value);
	}
	inline Snapshot parseSnapshot(const JsonObject& message)
	{
		if (message.GetNamedString(L"type") != L"snapshot" || integer(message, L"version") != 1)
		{
			throw winrt::hresult_invalid_argument(L"Expected protocol v1 snapshot");
		}
		Snapshot result;
		result.activeId = integer(message, L"activeTabId", -1);
		const auto tabs = message.GetNamedArray(L"tabs");
		if (tabs.Size() > 2000) { throw winrt::hresult_invalid_argument(L"Maximum 2000 tabs exceeded"); }
		std::set<int> seen;
		for (const auto& value : tabs)
		{
			const auto tab = value.GetObject();
			const int id = integer(tab, L"id");
			const auto title = tab.GetNamedString(L"title");
			if (title.size() > 1024 || !seen.insert(id).second || std::wstring_view(title).find(L'\0') != std::wstring_view::npos)
			{
				throw winrt::hresult_invalid_argument(L"Invalid tab title or duplicate ID");
			}
			result.tabs.push_back({id, integer(tab, L"windowId"), title});
		}
		if (result.activeId != -1 && !seen.contains(result.activeId))
		{
			throw winrt::hresult_invalid_argument(L"Active tab is absent from snapshot");
		}
		return result;
	}
	inline JsonObject message(const wchar_t* type)
	{
		JsonObject result;
		result.SetNamedValue(L"type", JsonValue::CreateStringValue(type));
		result.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
		return result;
	}
}
