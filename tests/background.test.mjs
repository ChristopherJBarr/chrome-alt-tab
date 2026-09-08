import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";

function event() {
	const listeners = [];
	return { addListener: listener => listeners.push(listener), emit: (...args) => listeners.map(listener => listener(...args)) };
}

let serial = 0;
async function worker(saved = {}, options = {}) {
	const connections = [];
	const badges = [];
	const session = { ...saved };
	const alarms = new Map();
	const timers = new Map();
	let timerId = 0;
	globalThis.setTimeout = function(callback, delay) {
		if (this !== globalThis) throw new TypeError("Illegal invocation");
		if (options.failTimer) { options.failTimer = false; throw new Error("Timer setup failed"); }
		timers.set(++timerId, { callback, delay }); return timerId;
	};
	globalThis.clearTimeout = function(id) {
		if (this !== globalThis) throw new TypeError("Illegal invocation");
		return timers.delete(id);
	};
	const api = {
		runtime: { onStartup: event(), onMessage: event(), connectNative: name => {
			assert.equal(name, "org.chrome_alt_tab.host");
			const port = { onMessage: event(), onDisconnect: event(), sent: [], disconnected: false, postMessage(message) { this.sent.push(message); }, disconnect() { this.disconnected = true; this.onDisconnect.emit(); } };
			connections.push(port);
			return port;
		} },
		action: { onClicked: event(), setTitle: async () => {}, setBadgeText: async ({ text }) => badges.push(text), setBadgeBackgroundColor: async () => {} },
		permissions: { contains: async () => false, onAdded: event(), onRemoved: event() },
		storage: { onChanged: event(), local: { get: async () => ({}) }, session: { get: options.getSaved ?? (async () => ({ ...session })), set: async value => Object.assign(session, value) } },
		alarms: { onAlarm: event(), create: async (name, value) => alarms.set(name, value), clear: async name => alarms.delete(name) },
		tabs: {}, windows: {}
	};
	for (const name of ["onCreated", "onRemoved", "onUpdated", "onActivated", "onAttached", "onDetached", "onMoved", "onReplaced"]) api.tabs[name] = event();
	for (const name of ["onFocusChanged", "onRemoved", "onCreated"]) api.windows[name] = event();
	api.windows.getAll = async () => [{ id: 1, type: "normal", focused: true, tabs: [{ id: 5, title: "test", active: true }] }];
	globalThis.chrome = api;
	if (options.synchronousLoad) {
		// Node's synchronous ESM require rejects an async module graph, including
		// top-level await in dependencies, just as Chrome service workers do.
		createRequire(import.meta.url)("../extension/background.js");
	} else {
		await import(`../extension/background.js?test=${serial++}`);
	}
	if (!options.getSaved) await new Promise(resolve => setImmediate(resolve));
	return { api, connections, session, badges, alarms, timers,
		fireTimers: async delay => {
			for (const [id, timer] of [...timers]) {
				if (timer.delay === delay) { timers.delete(id); timer.callback(); }
			}
			await settle();
		},
		fireAlarm: async () => {
			const name = "chrome-alt-tab-reconnect";
			const alarm = alarms.get(name);
			assert.ok(alarm, "retry alarm must exist");
			alarms.delete(name);
			const originalNow = Date.now;
			Date.now = () => alarm.when + 1;
			try { api.alarms.onAlarm.emit({ name }); await settle(); }
			finally { Date.now = originalNow; }
		}
	};
}

const settle = async () => { for (let i = 0; i < 6; i++) await new Promise(resolve => setImmediate(resolve)); };

test("service-worker module graph can load synchronously without top-level await", async () => {
	const { connections } = await worker({}, { synchronousLoad: true });
	assert.equal(connections.length, 1);
});

test("a click during saved-pause initialization reconnects after initialization", async () => {
	let release;
	const { api, connections, session } = await worker({}, {
		getSaved: () => new Promise(resolve => { release = resolve; })
	});
	const click = Promise.all(api.action.onClicked.emit());
	assert.equal(connections.length, 0);
	release({ paused: true });
	await click;
	assert.equal(connections.length, 1);
	assert.equal(session.paused, false);
});

test("worker waits for host handshake then sends a full snapshot", async () => {
	const { api, connections, badges } = await worker();
	const port = connections[0];
	api.tabs.onUpdated.emit(5, { title: "updated" });
	await settle();
	assert.equal(port.sent.length, 0);
	port.onMessage.emit({ type: "ready", version: 1 });
	await settle();
	assert.equal(port.sent[0].tabs[0].id, 5);
	port.onMessage.emit({ type: "synced", version: 1, count: 1 });
	await settle();
	assert.equal(badges.at(-1), "1");
});

test("disconnect reports an error without relaunch loop; click reconnects", async () => {
	const { api, connections, badges } = await worker();
	api.runtime.lastError = { message: "Host not found" };
	connections[0].onDisconnect.emit();
	await settle();
	assert.equal(badges.at(-1), "!");
	api.tabs.onCreated.emit();
	await settle();
	assert.equal(connections.length, 1);
	await Promise.all(api.action.onClicked.emit());
	assert.equal(connections.length, 2);
	connections[0].onMessage.emit({ type: "ready", version: 1 });
	await settle();
	assert.equal(connections[1].sent.length, 0, "stale port must not trigger a snapshot");
});

test("closing helper persists pause across worker restart", async () => {
	const first = await worker();
	first.connections[0].onMessage.emit({ type: "paused", version: 1 });
	first.connections[0].onDisconnect.emit();
	await settle();
	assert.equal(first.session.paused, true);
	const second = await worker(first.session);
	assert.equal(second.connections.length, 0);
	await Promise.all(second.api.action.onClicked.emit());
	assert.equal(second.connections.length, 1);
	assert.equal(second.session.paused, false);
});

test("events during an in-flight query cause a fresh snapshot", async () => {
	const { api, connections } = await worker();
	const port = connections[0];
	let release;
	api.windows.getAll = () => new Promise(resolve => { release = resolve; });
	port.onMessage.emit({ type: "ready", version: 1 });
	await settle();
	api.tabs.onRemoved.emit(5);
	api.windows.getAll = async () => [];
	release([{ id: 1, type: "normal", tabs: [{ id: 5, title: "old" }] }]);
	await settle();
	assert.equal(port.sent.length, 1, "only one snapshot may await acknowledgement");
	port.onMessage.emit({ type: "synced", version: 1, count: 1 });
	await settle();
	assert.equal(port.sent.length, 2);
	assert.deepEqual(port.sent[1].tabs, []);
});

test("native activation is acknowledged and followed by reconciliation", async () => {
	const { api, connections } = await worker();
	const calls = [];
	api.tabs.get = async id => ({ id, windowId: 1 });
	api.tabs.update = async id => calls.push(["activate", id]);
	api.windows.get = async () => ({ type: "normal" });
	api.windows.update = async id => calls.push(["focus", id]);
	const port = connections[0];
	port.onMessage.emit({ type: "ready", version: 1 });
	await settle();
	port.onMessage.emit({ type: "activate", version: 1, tabId: 5 });
	await settle();
	assert.deepEqual(calls, [["activate", 5], ["focus", 1]]);
	assert.ok(port.sent.some(message => message.type === "result" && message.ok));
	assert.equal(port.sent.at(-1).type, "result");
});

test("identical updates are suppressed and irrelevant tab changes do not query Chrome", async () => {
	const { api, connections, fireTimers } = await worker();
	let queries = 0;
	const originalQuery = api.windows.getAll;
	api.windows.getAll = async (...args) => { queries++; return originalQuery(...args); };
	const port = connections[0];
	port.onMessage.emit({ type: "ready", version: 1 });
	await settle();
	port.onMessage.emit({ type: "synced", version: 1, count: 1 });
	for (let i = 0; i < 100; i++) api.tabs.onUpdated.emit(5, { audible: true });
	await fireTimers(50);
	assert.equal(queries, 1);
	for (let i = 0; i < 100; i++) api.tabs.onUpdated.emit(5, { title: "test" });
	await fireTimers(50);
	assert.equal(queries, 2, "title burst should coalesce into one query");
	assert.equal(port.sent.length, 1, "same data must not be sent again");
});

test("toolbar click pauses a connected host; resume starts a fresh connection", async () => {
	const { api, connections, session, badges, alarms } = await worker();
	connections[0].onMessage.emit({ type: "ready", version: 1 });
	await settle();
	await Promise.all(api.action.onClicked.emit());
	assert.equal(session.paused, true);
	assert.equal(badges.at(-1), "OFF");
	assert.equal(alarms.size, 0);
	await Promise.all(api.action.onClicked.emit());
	assert.equal(session.paused, false);
	assert.equal(connections.length, 2);
});

test("host reconnects with bounded backoff and stops after five failed retries", async () => {
	const { connections, alarms, fireAlarm, session } = await worker();
	for (let retry = 0; retry < 5; retry++) {
		connections.at(-1).onDisconnect.emit();
		await settle();
		assert.equal(session.retries, retry + 1);
		assert.equal(alarms.size, 1);
		await fireAlarm();
		assert.equal(connections.length, retry + 2);
	}
	connections.at(-1).onDisconnect.emit();
	await settle();
	assert.equal(alarms.size, 0);
	assert.equal(session.retryAt, 0);
	assert.match(session.status, /exhausted/);
});

test("saved retry schedule survives worker restart without an immediate relaunch", async () => {
	const { connections, alarms, fireAlarm } = await worker({ retries: 2, retryAt: Date.now() + 60000 });
	assert.equal(connections.length, 0);
	assert.equal(alarms.size, 1);
	await fireAlarm();
	assert.equal(connections.length, 1);
});

test("unresponsive startup disconnects and schedules recovery", async () => {
	const { connections, fireTimers, session, alarms } = await worker();
	assert.equal(connections.length, 1);
	await fireTimers(10000);
	assert.equal(session.connected, false);
	assert.equal(alarms.size, 1);
});

test("commands for IDs outside the mirrored snapshot cannot touch Chrome", async () => {
	const { api, connections } = await worker();
	let called = false;
	api.tabs.get = async () => { called = true; };
	const port = connections[0];
	port.onMessage.emit({ type: "ready", version: 1 });
	await settle();
	port.onMessage.emit({ type: "close", version: 1, tabId: 999 });
	await settle();
	assert.equal(called, false);
	assert.ok(port.sent.some(value => value.type === "result" && value.ok === false));
});

test("two profile workers keep ports, snapshots and pause state independent", async () => {
	const work = await worker();
	const personal = await worker();
	personal.api.windows.getAll = async () => [{ id: 2, type: "normal", tabs: [{ id: 50, title: "personal" }] }];
	work.connections[0].onMessage.emit({ type: "ready", version: 1 });
	personal.connections[0].onMessage.emit({ type: "ready", version: 1 });
	await settle();
	assert.equal(work.connections[0].sent[0].tabs[0].id, 5);
	assert.equal(personal.connections[0].sent[0].tabs[0].id, 50);
	await Promise.all(work.api.action.onClicked.emit());
	assert.equal(work.session.paused, true);
	assert.notEqual(personal.session.paused, true);
});


test("startup timers retain the browser global receiver", async () => {
	const p = await worker();
	assert.equal(p.connections.length, 1);
	assert.equal(p.alarms.size, 0, "startup must not fail with Illegal invocation");
	assert.ok([...p.timers.values()].some(timer => timer.delay === 10000));
	p.connections[0].onMessage.emit({ type: "ready", version: 1 });
	await settle();
	assert.equal(p.connections[0].sent[0].type, "snapshot");
});

test("a failure after opening the native port disconnects it before retry", async () => {
	const p = await worker({}, { failTimer: true });
	assert.equal(p.connections[0].disconnected, true);
	assert.equal(p.alarms.size, 1);
	await Promise.all(p.api.action.onClicked.emit());
	assert.equal(p.connections.length, 2);
	assert.equal(p.connections[1].disconnected, false);
});


test("preview permission state is sent only to a host advertising support", async () => {
	for (const supported of [false, true]) {
		const { connections, api } = await worker();
		const port = connections[0];
		port.onMessage.emit({ type: "ready", version: 1, hostVersion: "0.4.0", previewState: supported });
		await settle();
		port.onMessage.emit({ type: "synced", version: 1, count: 1, activeTabId: 5 });
		await settle();
		assert.deepEqual(port.sent.filter(m => m.type === "preview_state"), supported ? [{ type: "preview_state", version: 1, enabled: false }] : []);
		api.permissions.contains = async () => true;
		api.permissions.onAdded.emit(); await settle();
		assert.equal(port.sent.filter(m => m.type === "preview_state" && m.enabled).length, supported ? 1 : 0);
	}
});

test("Chrome startup reconnects a session paused by Quit", async () => {
	const { api, connections, session } = await worker({ paused: true });
	assert.equal(connections.length, 0);
	api.runtime.onStartup.emit();
	await settle();
	assert.equal(connections.length, 1);
	assert.equal(session.paused, false);
});
