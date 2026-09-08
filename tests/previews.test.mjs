import test from "node:test";
import assert from "node:assert/strict";
import { createPreviews } from "../extension/previews.js";

const event = () => {
	const listeners = [];
	return { addListener: f => listeners.push(f), emit: (...args) => listeners.forEach(f => f(...args)) };
};
const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(resolve => setImmediate(resolve)); };
async function setup({ enabled = true, allowed = true, capture } = {}) {
	const sent = [];
	const permissionStates = [];
	const pending = new Map();
	let sequence = 0, now = 0, calls = 0;
	const local = { previewsEnabled: enabled };
	const window = { id: 1, focused: true, type: "normal", tabs: [{ id: 5, active: true, url: "https://example.com", status: "complete" }] };
	const api = {
		storage: { local: { get: async () => local }, onChanged: event() },
		permissions: { contains: async () => allowed, onRemoved: event(), onAdded: event() },
		windows: { getLastFocused: async () => structuredClone(window), onFocusChanged: event() },
		tabs: { onActivated: event(), onUpdated: event(), onRemoved: event(),
			captureVisibleTab: async () => { calls++; return capture ? capture() : "image"; } }
	};
	const previews = createPreviews(api, value => sent.push(value), () => true, async () => "jpeg-base64", () => now,
		{ set: (f, delay) => { pending.set(++sequence, { f, delay }); return sequence; }, clear: id => pending.delete(id) }, enabled => permissionStates.push(enabled));
	await previews.refresh();
	return { api, previews, sent, permissionStates, window, local, pending, calls: () => calls, revoke: () => { allowed = false; api.permissions.onRemoved.emit(); },
		fire: async () => { const jobs = [...pending.values()]; pending.clear(); for (const job of jobs) { now += job.delay; job.f(); } await settle(); } };
}

test("capture requires permission; a stale saved flag cannot suppress a granted opt-in", async () => {
	const denied = await setup({ allowed: false }); await denied.fire();
	assert.equal(denied.calls(), 0);
	const granted = await setup({ enabled: false, allowed: true }); await granted.fire();
	assert.equal(granted.calls(), 1);
});
test("only focused normal web pages can be captured", async () => {
	for (const mutate of [w => w.focused = false, w => w.incognito = true,
		w => w.type = "popup", w => w.tabs[0].url = "chrome://settings/",
		w => w.tabs[0].url = "file:///example", w => w.tabs[0].incognito = true,
		w => w.tabs[0].discarded = true, w => w.tabs[0].status = "loading"]) {
		const p = await setup(); mutate(p.window); await p.fire(); assert.equal(p.calls(), 0);
	}
});
test("captures one bounded image and sends no URL", async () => {
	const p = await setup(); await p.fire();
	assert.deepEqual(p.sent, [{ type: "preview", version: 1, tabId: 5, image: "jpeg-base64" }]);
	p.api.tabs.onActivated.emit();
	assert.equal([...p.pending.values()][0].delay, 2000);
});
test("a switch during capture discards the image even when the tab switches back", async () => {
	let release;
	const p = await setup({ capture: () => new Promise(resolve => { release = resolve; }) });
	await p.fire(); p.api.tabs.onActivated.emit(); release("image"); await settle();
	assert.equal(p.sent.length, 0);
});
test("URL changes during capture cannot be assigned to the new page", async () => {
	let release;
	const p = await setup({ capture: () => new Promise(resolve => { release = resolve; }) });
	await p.fire(); p.window.tabs[0].url = "https://other.example"; release("image"); await settle();
	assert.equal(p.sent.length, 0);
});
test("revoking permission immediately clears images and cancels an in-flight result", async () => {
	let release;
	const p = await setup({ capture: () => new Promise(resolve => { release = resolve; }) });
	await p.fire(); p.api.permissions.onRemoved.emit(); release("image"); await settle();
	assert.deepEqual(p.sent, [{ type: "clear_previews", version: 1 }]);
});
test("navigation clears the previous page and disabling clears the whole cache", async () => {
	const p = await setup(); await p.fire();
	p.api.tabs.onUpdated.emit(5, { status: "loading" });
	assert.deepEqual(p.sent.at(-1), { type: "clear_previews", version: 1, tabId: 5 });
	p.revoke(); await p.previews.refresh();
	assert.deepEqual(p.sent.at(-1), { type: "clear_previews", version: 1 });
});


test("permission revocation wins over an older pending grant check", async () => {
	const p = await setup();
	let release;
	p.api.permissions.contains = () => new Promise(resolve => { release = resolve; });
	const refresh = p.previews.refresh();
	p.revoke(); release(true); await refresh; await p.fire();
	assert.equal(p.calls(), 0);
});


test("preview permission reporting distinguishes disabled from uncaptured and revoked", async () => {
	const denied = await setup({ allowed: false });
	assert.deepEqual(denied.permissionStates, [false]);
	const enabled = await setup();
	enabled.window.tabs[0].url = "chrome://extensions/";
	await enabled.fire();
	assert.equal(enabled.calls(), 0);
	assert.deepEqual(enabled.permissionStates, [true]);
	enabled.revoke();
	assert.deepEqual(enabled.permissionStates, [true, false]);
});
