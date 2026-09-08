import test from "node:test";
import assert from "node:assert/strict";
import { makeSnapshot, applyCommand } from "../extension/model.js";

const normal = (id, tabs, focused = false) => ({ id, type: "normal", focused, tabs });
test("snapshot includes normal tabs across windows without URLs or incognito content", () => {
	const data = makeSnapshot([
		normal(1, [{ id: 10, title: "GitHub", active: true, lastAccessed: 20, url: "secret" }], true),
		normal(2, [{ id: 20, title: "日本語 🧪", lastAccessed: 10 }]),
		{ ...normal(3, [{ id: 30, title: "private" }]), incognito: true },
		{ id: 4, type: "popup", tabs: [{ id: 40 }] }
	]);
	assert.deepEqual(data, { type: "snapshot", version: 1, activeTabId: 10, focused: true, tabs: [
		{ id: 20, windowId: 2, lastAccessed: 10, title: "日本語 🧪" }, { id: 10, windowId: 1, lastAccessed: 20, title: "GitHub" }
	] });
});
test("previous active survives focus leaving Chrome; removed active picks recent survivor", () => {
	const windows = [normal(1, [{ id: 1, title: "a", lastAccessed: 1 }, { id: 2, title: "b", lastAccessed: 2 }])];
	assert.equal(makeSnapshot(windows, 1).activeTabId, 1);
	assert.equal(makeSnapshot(windows, 9).activeTabId, 2);
	assert.equal(makeSnapshot([], 2).activeTabId, -1);
});
test("titles are bounded and contain no null characters", () => {
	const snapshot = makeSnapshot([normal(1, [{ id: 1, title: "a\0" + "b".repeat(2000) }])]);
	assert.equal(snapshot.tabs[0].title.length, 1024);
	assert.ok(!snapshot.tabs[0].title.includes("\0"));
});
test("activation selects a tab then focuses its Chrome window", async () => {
	const calls = [];
	const api = {
		tabs: { get: async id => ({ id, windowId: 12 }), update: async (...args) => calls.push(["tab", ...args]) },
		windows: { get: async () => ({ type: "normal" }), update: async (...args) => calls.push(["window", ...args]) }
	};
	await applyCommand(api, { version: 1, type: "activate", tabId: 5 });
	assert.deepEqual(calls, [["tab", 5, { active: true }], ["window", 12, { focused: true }]]);
});
test("close commands only close the requested normal tab", async () => {
	const removed = [];
	const api = {
		tabs: { get: async id => ({ id, windowId: 1 }), remove: async id => removed.push(id) },
		windows: { get: async () => ({ type: "normal" }) }
	};
	await applyCommand(api, { version: 1, type: "close", tabId: 7 });
	assert.deepEqual(removed, [7]);
	api.windows.get = async () => ({ type: "normal", incognito: true });
	await assert.rejects(applyCommand(api, { version: 1, type: "close", tabId: 7 }));
	assert.deepEqual(removed, [7]);
});
test("malformed commands cannot invoke browser APIs", async () => {
	for (const message of [{}, { version: 1, type: "open", tabId: 1 }, { version: 2, type: "close", tabId: 1 }, { version: 1, type: "close", tabId: -1 }]) {
		await assert.rejects(applyCommand({}, message));
	}
});

test("focus leaving Chrome is explicit and does not invent a new tab visit", () => {
	const tabs = [{ id: 1, title: "a", active: true, lastAccessed: 42 }];
	assert.equal(makeSnapshot([normal(1, tabs, true)]).focused, true);
	const blurred = makeSnapshot([normal(1, tabs, false)], 1);
	assert.equal(blurred.focused, false);
	assert.equal(blurred.activeTabId, 1);
	assert.equal(blurred.tabs[0].lastAccessed, 42);
});
test("invalid visit timestamps cannot poison the combined MRU", () => {
	for (const lastAccessed of [NaN, Infinity, -1, "100"])
		assert.equal(makeSnapshot([normal(1, [{ id: 1, lastAccessed }])]).tabs[0].lastAccessed, 0);
});
