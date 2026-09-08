// Full snapshots allow recovery after service-worker or native-host restarts.
export function makeSnapshot(windows, previousActiveId = -1) {
	const tabs = windows.filter(window => window.type === "normal" && !window.incognito)
		.flatMap(window => (window.tabs ?? []).filter(tab => !tab.incognito && Number.isInteger(tab.id))
			.map(tab => ({
				id: tab.id,
				windowId: window.id,
				title: (tab.title || "Untitled Chrome tab").replaceAll("\0", "").slice(0, 1024).replace(/[\uD800-\uDBFF]$/, ""),
				lastAccessed: Number.isFinite(tab.lastAccessed) && tab.lastAccessed >= 0 ? Math.min(tab.lastAccessed, Date.now()) : 0,
				focused: window.focused && tab.active
			})));
	if (tabs.length > 2000) throw new Error("This prototype supports up to 2000 tabs.");
	tabs.sort((a, b) => a.lastAccessed - b.lastAccessed || a.id - b.id);
	const activeTabId = tabs.find(tab => tab.focused)?.id
		?? (tabs.some(tab => tab.id === previousActiveId) ? previousActiveId : tabs.at(-1)?.id ?? -1);
	return {
		type: "snapshot", version: 1, activeTabId, focused: tabs.some(tab => tab.focused),
		tabs: tabs.map(({ id, windowId, title, lastAccessed }) => ({ id, windowId, title, lastAccessed }))
	};
}

export async function applyCommand(chrome, message) {
	if (message.version !== 1 || !["activate", "close"].includes(message.type)
		|| !Number.isInteger(message.tabId) || message.tabId < 0) {
		throw new Error("Invalid native tab command.");
	}
	const tab = await chrome.tabs.get(message.tabId);
	const window = await chrome.windows.get(tab.windowId);
	if (tab.incognito || window.incognito || window.type !== "normal") {
		throw new Error("Tab is outside the normal-window scope.");
	}
	if (message.type === "close") {
		await chrome.tabs.remove(tab.id);
	} else {
		await chrome.tabs.update(tab.id, { active: true });
		await chrome.windows.update(tab.windowId, { focused: true });
	}
}
