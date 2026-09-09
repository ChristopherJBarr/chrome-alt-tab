const toggle = document.getElementById("toggle");
const previews = document.getElementById("previews");
let enabled = false;
async function render() {
	const [session, allowed] = await Promise.all([
		chrome.storage.session.get(["status", "connected", "hostVersion", "previewStatus"]),
		chrome.permissions.contains({ origins: ["<all_urls>"] })
	]);
	document.getElementById("status").textContent = session.status || "Starting…";
	toggle.textContent = session.connected ? "Pause" : "Resume / reconnect";
	toggle.disabled = false;
	enabled = allowed;
	previews.textContent = enabled ? "Disable previews" : "Enable previews…";
	previews.disabled = !session.connected || !["0.4.0", "0.5.0", "0.5.1"].includes(session.hostVersion);
	document.getElementById("preview-status").textContent = enabled
		? (session.previewStatus || "Enabled. Visit a web tab and wait a moment to capture it. Up to 48 recent previews are kept per profile.")
		: (session.connected && !["0.4.0", "0.5.0", "0.5.1"].includes(session.hostVersion) ? "Helper update required. Reconnect after registering the new build." : "Off. Tabs use a placeholder until you enable previews and visit them.");
	document.getElementById("version").textContent = `Extension ${chrome.runtime.getManifest().version} · Helper ${session.hostVersion || "not connected"}`;
}
toggle.addEventListener("click", async () => {
	toggle.disabled = true;
	try { await chrome.runtime.sendMessage({ type: "toggle" }); await render(); }
	catch (error) { document.getElementById("status").textContent = error.message; toggle.disabled = false; }
});
previews.addEventListener("click", async () => {
	const disabling = enabled;
	try {
		// Request directly inside the gesture, before any unrelated await.
		if (!disabling && !await chrome.permissions.request({ origins: ["<all_urls>"] })) return;
		if (disabling) await chrome.permissions.remove({ origins: ["<all_urls>"] });
		await render();
	} catch (error) { document.getElementById("preview-status").textContent = error.message; }
});
chrome.storage.onChanged.addListener(() => { void render(); });
void render();
