#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace switcher
{
	struct CardRect { int left, top, right, bottom; };
	struct Layout { int width = 0, height = 0; std::vector<CardRect> cards; };
	// Sizes are DIPs. Keep preview height consistent while respecting aspect ratios.
	inline Layout layoutCards(const std::vector<double>& aspects, int maxWidth, int maxHeight)
	{
		Layout result;
		if (aspects.empty()) { return result; }
		maxWidth = std::max(160, maxWidth); maxHeight = std::max(100, maxHeight);
		for (int previewHeight = 134; previewHeight >= 24; --previewHeight)
		{
			result = {}; int x = 16, y = 16; size_t rowStart = 0;
			const int cardHeight = previewHeight + 34;
			std::vector<std::pair<size_t, int>> rows;
			for (double aspect : aspects)
			{
				if (!std::isfinite(aspect) || aspect <= 0) { aspect = 1.6; }
				const int w = std::min(maxWidth - 32, std::clamp(static_cast<int>(previewHeight * std::clamp(aspect, 0.1, 10.0)), std::max(48, previewHeight * 130 / 134), 270));
				if (x > 16 && x + w + 16 > maxWidth)
				{
					rows.emplace_back(rowStart, x - 24); rowStart = result.cards.size();
					result.width = std::max(result.width, x - 8); x = 16; y += cardHeight + 24;
				}
				result.cards.push_back({x, y, x + w, y + cardHeight}); x += w + 24;
			}
			rows.emplace_back(rowStart, x - 24); result.width = std::max(result.width, x - 8);
			result.height = y + cardHeight + 16;
			for (size_t row = 0; row < rows.size(); ++row)
			{
				const size_t end = row + 1 < rows.size() ? rows[row + 1].first : result.cards.size();
				const int shift = (result.width - 16 - rows[row].second) / 2;
				for (size_t i = rows[row].first; i < end; ++i) { result.cards[i].left += shift; result.cards[i].right += shift; }
			}
			if (result.height <= maxHeight) { break; }
		}
		return result;
	}
	inline size_t verticalCard(const std::vector<CardRect>& cards, size_t selected, int direction)
	{
		if (selected >= cards.size()) { return selected; }
		const auto& current = cards[selected];
		int targetRow = direction < 0 ? -1 : INT32_MAX;
		for (const auto& r : cards)
		{
			if (direction < 0 && r.top < current.top) { targetRow = std::max(targetRow, r.top); }
			if (direction > 0 && r.top > current.top) { targetRow = std::min(targetRow, r.top); }
		}
		if (targetRow == -1 || targetRow == INT32_MAX) { targetRow = direction < 0 ? cards.back().top : cards.front().top; }
		size_t best = selected; int distance = INT32_MAX;
		for (size_t i = 0; i < cards.size(); ++i)
		{
			const auto& r = cards[i]; const int delta = std::abs(r.left + r.right - current.left - current.right);
			if (r.top == targetRow && delta < distance) { best = i; distance = delta; }
		}
		return best;
	}

}
