#include "switcher_model.h"
#include "switcher_layout.h"
#include <iostream>
#include <stdexcept>
void require(bool condition) { if (!condition) { throw std::runtime_error("MRU test failed"); } }
int main()
{
	require(switcher::chromeTitleMatches(L"GitHub - Google Chrome", L"GitHub"));
	require(!switcher::chromeTitleMatches(L"GitHub Issues - Google Chrome", L"GitHub"));
	require(!switcher::chromeTitleMatches(L"GitHub - Issues - Google Chrome", L"GitHub"));
	require(!switcher::chromeTitleMatches(L"GitHub - Google Chrome", L""));
	require(!switcher::chromeTitleMatches(L"GitHub", L"GitHub"));
	switcher::Recent recent;
	recent.seed({1, 5}, 100); recent.seed({2, 5}, 200);
	recent.seed({1, 5}, 900); require(recent.get({1, 5}) == 100);
	recent.visit({1, 5}, 300); require(recent.get({1, 5}) > recent.get({2, 5}));
	recent.visit({2, 5}, 300); require(recent.get({2, 5}) > recent.get({1, 5}));
	recent.visit({0, 42}, 299); require(recent.get({0, 42}) > recent.get({2, 5}));
	recent.eraseClient(1); require(!recent.times.contains({1, 5}) && recent.times.contains({2, 5}));
	require(switcher::cycle(0, 3, -1) == 2); require(switcher::cycle(2, 3, 1) == 0);
	require(switcher::cycle(0, 0, 1) == 0); require(switcher::cycle(0, 1, -1) == 0);
	for (int count = 1; count <= 24; ++count)
	{
		std::vector<double> aspects;
		for (int i = 0; i < count; ++i) { aspects.push_back(i % 2 ? 0.6 : 1.8); }
		for (const auto [w, h] : {std::pair{1420, 900}, std::pair{600, 420}})
		{
			const auto layout = switcher::layoutCards(aspects, w, h);
			require(layout.width <= w && layout.height <= h && layout.cards.size() == aspects.size());
			for (size_t i = 0; i < layout.cards.size(); ++i)
			{
				const auto& a = layout.cards[i];
				require(a.left >= 0 && a.top >= 0 && a.right <= layout.width && a.bottom <= layout.height);
				for (size_t j = i + 1; j < layout.cards.size(); ++j)
				{
					const auto& b = layout.cards[j];
					require(a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top);
				}
			}
		}
	}
	const std::vector<switcher::CardRect> navigation{{0,0,100,100},{120,0,300,100},{0,120,180,220},{200,120,300,220}};
	require(switcher::verticalCard(navigation, 1, 1) == 3);
	require(switcher::verticalCard(navigation, 2, -1) == 0);
	require(switcher::verticalCard(navigation, 0, -1) == 2);
	require(switcher::verticalCard({}, 0, 1) == 0);
	const auto portrait = switcher::layoutCards({0.6, 1.8}, 1400, 900);
	require(portrait.cards[0].right - portrait.cards[0].left < portrait.cards[1].right - portrait.cards[1].left);
	std::cout << "MRU isolation, refresh stability, monotonic visits and cycling passed\n";
}
