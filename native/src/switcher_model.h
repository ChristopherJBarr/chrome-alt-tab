#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace switcher
{
	inline bool chromeTitleMatches(std::wstring_view windowTitle, std::wstring_view tabTitle)
	{
		return !tabTitle.empty() && windowTitle.starts_with(tabTitle)
			&& windowTitle.substr(tabTitle.size()) == L" - Google Chrome";
	}
	struct Key
	{
		std::uint64_t client = 0;
		std::uint64_t id = 0;
		auto operator<=>(const Key&) const = default;
	};
	struct Recent
	{
		std::map<Key, std::uint64_t> times;
		std::uint64_t clock = 0;
		void seed(Key key, std::uint64_t time) { times.try_emplace(key, time); }
		void visit(Key key, std::uint64_t time) { clock = std::max(clock + 1, time); times[key] = clock; }
		void eraseClient(std::uint64_t client) { std::erase_if(times, [client](const auto& item) { return item.first.client == client; }); }
		std::uint64_t get(Key key) const { const auto it = times.find(key); return it == times.end() ? 0 : it->second; }
	};
	inline size_t cycle(size_t selected, size_t count, int direction)
	{
		if (!count) { return 0; }
		return direction < 0 ? (selected + count - 1) % count : (selected + 1) % count;
	}
}
