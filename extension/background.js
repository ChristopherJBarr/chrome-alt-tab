import { makeSnapshot, applyCommand } from "./model.js";
import { createPreviews } from "./previews.js";

const chrome = globalThis.chrome;
const timers = { set: globalThis.setTimeout.bind(globalThis), clear: globalThis.clearTimeout.bind(globalThis) };
const hostName = "org.chrome_alt_tab.host";
const retryAlarm = "chrome-alt-tab-reconnect";
const retryDelays = [30, 60, 120, 240, 300];
let port = null;
let ready = false;
let previewCapable = false;
let previewStateCapable = false;
let paused = false;
let syncing = false;
let dirty = false;
let awaitingSnapshot = false;
let lastSnapshot = "";
let mirroredIds = new Set();
let syncTimer = null;
let handshakeTimer = null;
let retries = 0;
let retryAt = 0;
let connectedSince = 0;
let activeTabId = -1;
let commandQueue = Promise.resolve();
const previews = createPreviews(chrome,
	message => port?.postMessage(message), () => ready && previewCapable && !paused, undefined, undefined, timers,
	enabled => { if (ready && previewStateCapable && !paused) port?.postMessage({ type: "preview_state", version: 1, enabled }); });

async function status(text, badge, error = false) {
	await Promise.all([
		chrome.action.setTitle({ title: `Chrome Alt+Tab: ${text}` }),
		chrome.action.setBadgeText({ text: badge }),
		chrome.action.setBadgeBackgroundColor({ color: error ? "#B42318" : "#176B42" }),
		chrome.storage.session.set({ status: text, connected: ready, paused })
	]).catch(error => console.error("Cannot update Chrome Alt+Tab status:", error));
}

async function flush() {
	if (syncing || awaitingSnapshot || !port || !ready) return;
	syncing = true;
	try {
		while (dirty && port && ready && !awaitingSnapshot) {
			dirty = false;
			const connection = port;
			const windows = await chrome.windows.getAll({ populate: true, windowTypes: ["normal"] });
			if (port !== connection || !ready) { dirty = true; break; }
			const snapshot = makeSnapshot(windows, activeTabId);
			activeTabId = snapshot.activeTabId;
			const json = JSON.stringify(snapshot);
			if (json === lastSnapshot) continue;
			if (new TextEncoder().encode(json).byteLength > 1024 * 1024) {
				throw new Error("Tab information exceeds the prototype's message limit.");
			}
			connection.postMessage(snapshot);
			lastSnapshot = json;
			mirroredIds = new Set(snapshot.tabs.map(tab => tab.id));
			awaitingSnapshot = true;
		}
	} catch (error) {
		await status(error.message, "!", true);
	} finally {
		syncing = false;
		if (dirty && port && ready && !awaitingSnapshot) scheduleSnapshot();
	}
}

function scheduleSnapshot() {
	dirty = true;
	if (syncTimer !== null) return;
	syncTimer = timers.set(() => { syncTimer = null; void flush(); }, 50);
}

async function persistRetry() {
	await chrome.storage.session.set({ retries, retryAt });
}

async function disconnected(error) {
	if (paused) {
		await chrome.alarms.clear(retryAlarm);
		return status("paused; click to resume", "OFF");
	}
	if (retries >= retryDelays.length) {
		retryAt = 0;
		await persistRetry();
		return status(`${error}; automatic retries exhausted; click to retry`, "!", true);
	}
	const delay = retryDelays[retries++];
	retryAt = Date.now() + delay * 1000;
	await persistRetry();
	if (paused || port) return;
	await chrome.alarms.create(retryAlarm, { when: retryAt });
	await status(`${error}; retrying in ${delay}s; click to retry now`, "!", true);
}

function reportFailure(error) {
	console.error("Chrome Alt+Tab:", error);
	void status(error.message || String(error), "!", true);
}

function connect() {
	if (port || paused) return;
	let connection = null;
	try {
		connection = chrome.runtime.connectNative(hostName);
		port = connection;
		ready = false;
		previewCapable = false;
		previewStateCapable = false;
		previews.reset();
		lastSnapshot = "";
		awaitingSnapshot = false;
		mirroredIds = new Set();
		void chrome.storage.session.set({ previewStatus: "Waiting for a web tab capture." });
		void status("connecting", "...");
		handshakeTimer = timers.set(() => {
			if (port === connection && !ready) connection.disconnect();
		}, 10000);
		connection.onDisconnect.addListener(() => {
			const error = chrome.runtime.lastError?.message;
			if (port !== connection) return;
			timers.clear(handshakeTimer);
			handshakeTimer = null;
			if (ready && Date.now() - connectedSince >= 60000) retries = 0;
			port = null;
			ready = false;
			previews.reset();
			awaitingSnapshot = false;
			void disconnected(error || "native host disconnected").catch(reportFailure);
		});
		connection.onMessage.addListener(message => {
			if (port !== connection || !message || message.version !== 1) return;
			if (message.type === "ready") {
				if (ready) return;
				timers.clear(handshakeTimer);
				handshakeTimer = null;
				ready = true;
				previewCapable = ["0.4.0", "0.5.0", "0.5.1"].includes(message.hostVersion);
				previewStateCapable = message.previewState === true;
				connectedSince = Date.now();
				retryAt = 0;
				void persistRetry().catch(reportFailure);
				void chrome.alarms.clear(retryAlarm);
				void chrome.storage.session.set({ hostVersion: message.hostVersion || "0.2.x" });
				dirty = true;
				void flush();
			} else if (message.type === "synced") {
				if (!Number.isInteger(message.count) || message.count < 0 || message.count > 2000) return;
				awaitingSnapshot = false;
				void status(`connected; ${message.count} tabs`, String(message.count));
				void previews.refresh().catch(reportFailure);
				void flush();
			} else if (message.type === "previewed") {
				if (Number.isInteger(message.tabId) && mirroredIds.has(message.tabId)) {
					void chrome.storage.session.set({ previewStatus: "A preview reached the helper. Visit other web tabs to capture them too." });
				}
			} else if (message.type === "paused") {
				paused = true;
				void chrome.storage.session.set({ paused: true }).catch(reportFailure);
				void chrome.alarms.clear(retryAlarm);
			} else if (message.type === "error") {
				void status(message.message || "native host error", "!", true);
			} else if (["activate", "close"].includes(message.type)) {
				commandQueue = commandQueue.then(async () => {
					if (port !== connection) return;
					let ok = false;
					try {
						if (!ready || !mirroredIds.has(message.tabId)) throw new Error("Tab is no longer mirrored.");
						await applyCommand(chrome, message);
						ok = true;
					}
					catch (error) { await status(error.message, "!", true); }
					if (port === connection) connection.postMessage({ type: "result", version: 1, ok });
					scheduleSnapshot();
				}).catch(error => { void status(error.message, "!", true); });
			}
		});
	} catch (error) {
		// Startup can fail after Chrome has already launched the host. Close the
		// partial connection so retries cannot leave orphan helper windows.
		port = null;
		ready = false;
		awaitingSnapshot = false;
		timers.clear(handshakeTimer);
		handshakeTimer = null;
		if (connection) connection.disconnect();
		void disconnected(error.message).catch(reportFailure);
	}
}

chrome.tabs.onUpdated.addListener((_tabId, changes) => {
	if (Object.hasOwn(changes, "title")) scheduleSnapshot();
});
for (const event of [chrome.tabs.onCreated, chrome.tabs.onRemoved,
	chrome.tabs.onActivated, chrome.tabs.onAttached, chrome.tabs.onDetached,
	chrome.tabs.onMoved, chrome.tabs.onReplaced, chrome.windows.onFocusChanged,
	chrome.windows.onRemoved, chrome.windows.onCreated]) {
	event.addListener(scheduleSnapshot);
}

async function toggleConnection() {
	try {
		await initialization;
		await chrome.alarms.clear(retryAlarm);
		retries = 0;
		retryAt = 0;
		if (port && ready) {
			paused = true;
			const connection = port;
			port = null;
			ready = false;
			previews.reset();
			awaitingSnapshot = false;
			connection.disconnect();
			await status("paused; click to resume", "OFF");
		} else {
			paused = false;
			await chrome.storage.session.set({ paused: false });
			connect();
		}
		await persistRetry();
	} catch (error) { reportFailure(error); }
}
chrome.action.onClicked.addListener(toggleConnection);
chrome.runtime.onMessage.addListener((message, sender, respond) => {
	if (sender.id !== chrome.runtime.id || message?.type !== "toggle") return;
	void toggleConnection().then(() => respond({ ok: true }));
	return true;
});

chrome.alarms.onAlarm.addListener(alarm => {
	if (alarm.name !== retryAlarm) return;
	void initialization.then(() => {
		if (paused || port) return;
		if (retryAt > Date.now()) return chrome.alarms.create(retryAlarm, { when: retryAt });
		retryAt = 0;
		connect();
	}).catch(reportFailure);
});

// A native messaging Port keeps a Manifest V3 service worker alive. Alarms make
// bounded reconnects survive idle-worker termination; no continuous polling.
// Chrome rejects service-worker module graphs containing top-level await. Keep
// listener registration synchronous and share the asynchronous startup barrier
// with clicks so a saved pause cannot overwrite a user's reconnect request.
const initialization = chrome.storage.session.get(["paused", "retries", "retryAt"]).then(saved => {
	paused = saved.paused === true;
	retries = Number.isInteger(saved.retries) && saved.retries >= 0 ? saved.retries : 0;
	retryAt = typeof saved.retryAt === "number" ? saved.retryAt : 0;
	if (paused) {
		void chrome.alarms.clear(retryAlarm);
		return status("paused; click to resume", "OFF");
	}
	if (retries >= retryDelays.length && retryAt === 0) return status("automatic retries exhausted; click to retry", "!", true);
	if (retryAt > Date.now()) return chrome.alarms.create(retryAlarm, { when: retryAt });
	connect();
}).catch(error => {
	console.error("Chrome Alt+Tab startup failed:", error);
	return status(`startup failed: ${error.message}`, "!", true);
});

// Wake the worker when Chrome starts, including when no tab event is emitted.
chrome.runtime.onStartup.addListener(() => {
	void initialization.then(async () => {
		paused = false; retries = 0; retryAt = 0;
		await chrome.storage.session.set({ paused: false, retries, retryAt });
		await chrome.alarms.clear(retryAlarm);
		connect();
	}).catch(reportFailure);
});
