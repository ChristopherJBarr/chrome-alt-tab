// Images are never written to extension storage. The host owns the bounded cache.
export async function shrinkPreview(dataUrl) {
	const bitmap = await createImageBitmap(await (await fetch(dataUrl)).blob());
	try {
		const scale = Math.min(1, 640 / bitmap.width, 400 / bitmap.height);
		const canvas = new OffscreenCanvas(Math.max(1, Math.round(bitmap.width * scale)), Math.max(1, Math.round(bitmap.height * scale)));
		canvas.getContext("2d").drawImage(bitmap, 0, 0, canvas.width, canvas.height);
		const blob = await canvas.convertToBlob({ type: "image/jpeg", quality: 0.65 });
		if (blob.size > 128 * 1024) throw new Error("Preview is too large");
		const bytes = new Uint8Array(await blob.arrayBuffer());
		let binary = "";
		for (const byte of bytes) binary += String.fromCharCode(byte);
		return btoa(binary);
	} finally { bitmap.close(); }
}

export function createPreviews(api, send, available, resize = shrinkPreview, clock = Date.now,
	timers = { set: globalThis.setTimeout.bind(globalThis), clear: globalThis.clearTimeout.bind(globalThis) }, permissionChanged = () => {}) {
	let enabled = false;
	let generation = 0;
	let configuration = 0;
	let timer = null;
	let busy = false;
	let lastCapture = -Infinity;
	const emit = value => { if (available()) send({ version: 1, ...value }); };
	async function refresh() {
		const request = ++configuration;
		const allowed = await api.permissions.contains({ origins: ["<all_urls>"] });
		if (request !== configuration) return;
		enabled = allowed;
		permissionChanged(enabled);
		generation++;
		if (!enabled) emit({ type: "clear_previews" });
		else schedule();
	}
	async function current() {
		const window = await api.windows.getLastFocused({ populate: true, windowTypes: ["normal"] });
		const tab = window.tabs?.find(tab => tab.active);
		if (!window.focused || window.incognito || window.type !== "normal" || !tab || tab.incognito ||
			tab.discarded || tab.status === "loading" || !/^https?:\/\//i.test(tab.url || "")) return null;
		return { id: tab.id, windowId: window.id, url: tab.url };
	}
	async function capture() {
		if (!enabled || !available()) return;
		if (busy) { schedule(); return; }
		const epoch = generation;
		busy = true;
		try {
			const before = await current();
			if (!before || epoch !== generation || !enabled || !available()) return;
			lastCapture = clock();
			const data = await api.tabs.captureVisibleTab(before.windowId, { format: "jpeg", quality: 65 });
			const image = await resize(data);
			const after = await current();
			if (epoch !== generation || !enabled || !available() || !after ||
				before.id !== after.id || before.windowId !== after.windowId || before.url !== after.url) return;
			emit({ type: "preview", tabId: before.id, image });
		} catch {
			// Protected pages and tabs closed during capture retain a placeholder.
			// Do not log image data or URLs, or interrupt normal tab switching.
		} finally { busy = false; }
	}
	function schedule() {
		generation++;
		if (timer !== null) timers.clear(timer);
		timer = null;
		if (!enabled || !available()) return;
		timer = timers.set(() => { timer = null; void capture(); }, Math.max(750, 2000 - (clock() - lastCapture)));
	}
	function invalidate(id) {
		generation++;
		emit({ type: "clear_previews", tabId: id });
		schedule();
	}
	api.tabs.onActivated.addListener(schedule);
	api.windows.onFocusChanged.addListener(schedule);
	api.tabs.onUpdated.addListener((id, changes) => {
		if (changes.url || changes.status === "loading") invalidate(id);
		else if (changes.status === "complete") schedule();
	});
	api.tabs.onRemoved.addListener(invalidate);
	// The grant itself is the opt-in. No popup-lifetime-dependent second flag.
	api.permissions.onAdded.addListener(() => { void refresh().catch(() => {}); });
	api.permissions.onRemoved.addListener(() => {
		configuration++;
		enabled = false;
		permissionChanged(false);
		generation++;
		emit({ type: "clear_previews" });
	});
	return { refresh, schedule, reset() { generation++; if (timer !== null) timers.clear(timer); timer = null; } };
}
