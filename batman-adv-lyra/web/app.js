// Polls /cgi-bin/status and hands the JSON to the page's render function.
const POLL_MS = 1000;
// batman-adv keeps a node that went quiet for 200 s (BATADV_PURGE_TIMEOUT); with an
// announcement every 250 ms (MESH_OGM_MS), 3 s of silence already means it is gone
const LOST_S = 3;
const isLost = (o) => o.last_seen >= LOST_S;

function $(id) {
	return document.getElementById(id);
}

function esc(s) {
	return String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
}

function fmtUptime(sec) {
	sec = Math.floor(sec);
	const d = Math.floor(sec / 86400);
	const h = Math.floor((sec % 86400) / 3600);
	const m = Math.floor((sec % 3600) / 60);
	if (d) return `${d}d ${h}h`;
	if (h) return `${h}h ${m}m`;
	return `${m}m ${sec % 60}s`;
}

function tqPercent(tq) {
	return Math.round((tq / 255) * 100);
}

function tqClass(tq) {
	if (tq >= 170) return "";
	if (tq >= 85) return "mid";
	return "low";
}

function setOnline(ok) {
	const dot = $("dot");
	dot.className = "dot " + (ok ? "ok" : "bad");
	dot.title = ok ? "connected to node" : "node not responding";
}

// Li-ion charge from the resting voltage of one cell: [volts, %], a typical discharge curve.
// Under load the voltage sags a little, so the estimate reads somewhat low while drawing current.
const LIION_CURVE = [
	[3.27, 0], [3.61, 5], [3.69, 10], [3.71, 15], [3.73, 20], [3.75, 25], [3.77, 30], [3.79, 35],
	[3.80, 40], [3.82, 45], [3.84, 50], [3.85, 55], [3.87, 60], [3.91, 65], [3.95, 70], [3.98, 75],
	[4.02, 80], [4.08, 85], [4.11, 90], [4.15, 95], [4.20, 100],
];

// Charge in % of a pack of `cells` Li-ion cells in series, null if the voltage is not a plausible pack
function batteryPercent(volts, cells) {
	if (!cells) return null;
	const cell = volts / cells;
	if (cell < 2.5 || cell > 4.35) return null;       // nothing connected, or not this many cells
	if (cell <= LIION_CURVE[0][0]) return 0;
	for (let i = 1; i < LIION_CURVE.length; i++) {
		const [v1, p1] = LIION_CURVE[i], [v0, p0] = LIION_CURVE[i - 1];
		if (cell <= v1) return Math.round(p0 + (p1 - p0) * (cell - v0) / (v1 - v0));
	}
	return 100;
}

// Battery readout in the header (ina226d), on every page that has #power: "8.35V (97% 2S)"
function fmtPower(p) {
	const cells = p.cells ?? 2;
	const pct = batteryPercent(p.bus_v, cells);
	const v = `${p.bus_v.toFixed(2)}V${pct != null ? ` (${pct}% ${cells}S)` : ""}`;
	const i = Math.abs(p.current_ma) >= 1000 ? `${(p.current_ma / 1000).toFixed(2)} A` : `${Math.round(p.current_ma)} mA`;
	const w = Math.abs(p.power_mw) >= 1000 ? `${(p.power_mw / 1000).toFixed(2)} W` : `${Math.round(p.power_mw)} mW`;
	return [v, i, w];
}

async function pollPower() {
	const el = $("power");
	if (!el) return;
	try {
		const res = await fetch("/cgi-bin/power", { cache: "no-store" });
		const p = await res.json();
		el.hidden = false;
		if (p.ok) {
			const [v, i, w] = fmtPower(p);
			el.className = "power";
			el.innerHTML = `<span>${v}</span><span>${i}</span><span>${w}</span>`;
			el.title = `Battery: ${v}, ${i}, ${w} (shunt ${p.shunt_mv.toFixed(2)} mV)`;
		} else {
			el.className = "power off";
			el.textContent = "battery n/a";
			el.title = p.error || "no battery data";
		}
	} catch (e) {
		el.hidden = true;
	}
	setTimeout(pollPower, POLL_MS);
}

// Internet status in the header (erl-inet), on every page. Pages can subscribe to the
// same data with onInternet(fn), e.g. the Node page's Internet card.
const internetListeners = [];
function onInternet(fn) {
	internetListeners.push(fn);
}

function inetLabel(i) {
	if (!i || i.role === null) return ["unknown", "off"];
	if (i.online === null) return ["checking…", "off"];
	if (!i.online) return [i.uplink || i.gateway ? "no internet" : "offline", "bad"];
	if (i.uplink) return [`via ${i.uplink.kind.startsWith("phone") ? "phone" : i.uplink.kind === "pc" ? "PC" : i.uplink.kind}`, "good"];
	if (i.gateway) return [`via ${i.gateway}`, "good"];
	return ["online", "good"];
}

async function pollInternet() {
	const el = $("inet");
	try {
		const res = await fetch("/cgi-bin/internet", { cache: "no-store" });
		const i = await res.json();
		const [text, cls] = inetLabel(i);
		el.hidden = false;
		el.className = "inet " + cls;
		el.innerHTML = `<span class="globe" aria-hidden="true"></span>${esc(text)}`;
		el.title = i.online
			? `Internet reachable${i.latency_ms != null ? `, ${i.latency_ms} ms` : ""}` +
			  (i.uplink ? ` through ${i.uplink.dev}` : i.gateway ? ` through mesh gateway ${i.gateway}` : "")
			: "No internet connection";
		internetListeners.forEach((fn) => fn(i));
	} catch (e) {
		el.hidden = true;
	}
	setTimeout(pollInternet, POLL_MS * 2);
}

// A compass as the unit sees it (its front up): the needle always points to North, so it turns
// by -heading; red tip = North. Inline SVG.
function compassRose(heading, size = 16) {
	return `<svg class="rose" viewBox="-10 -10 20 20" width="${size}" height="${size}" aria-hidden="true">
		<circle r="9" class="rose-ring"/><g transform="rotate(${-heading})">
		<path d="M0,-8 L2.8,0 L-2.8,0 Z" class="rose-north"/><path d="M0,8 L2.8,0 L-2.8,0 Z" class="rose-south"/></g></svg>`;
}

// Which way something faces on a north-up map: an arrow turned by +heading
function headingArrowIcon(heading, size = 12) {
	return `<svg class="dir-arrow" viewBox="-10 -10 20 20" width="${size}" height="${size}" aria-hidden="true">
		<path d="M0,-9 L5,4 L0,1 L-5,4 Z" transform="rotate(${heading})"/></svg>`;
}

// The unit's compass in the header (merld reads it every second; firmware with a QMC5883), or
// the phone's (sent by the app) for a unit built without one
async function pollCompass() {
	const el = $("compass");
	try {
		const res = await fetch("/cgi-bin/merl", { cache: "no-store" });
		const d = await res.json();
		const c = d.ok && d.state ? d.state.compass : null;
		el.hidden = !c;
		if (c) {
			// the rose points to North; the number is where the unit faces, measured from North
			const phone = c.source === "phone";
			el.innerHTML = `${compassRose(c.heading)}${Math.round(c.heading)}° ${esc(c.cardinal)}` +
				(phone ? ` <span class="src-tag">phone</span>` : "");
			el.title = `Facing ${c.heading.toFixed(1)}° from North (${c.cardinal}); the red needle points North. ` +
				(phone ? "From the phone's compass (the unit has none)." : `Raw x ${c.x}, y ${c.y}, z ${c.z}`);
		}
	} catch (e) {
		el.hidden = true;
	}
	setTimeout(pollCompass, POLL_MS);
}

// header: internet status right of the title; the compass and the battery readout share a
// line below it
document.addEventListener("DOMContentLoaded", () => {
	const h1 = document.querySelector("header h1");
	if (h1) {
		const group = document.createElement("span");
		group.className = "hstatus";
		group.innerHTML = `<span id="inet" class="inet off" hidden></span>`;
		h1.appendChild(group);
		// the second line: a block element, so no browser can lay it out next to the title
		const line = document.createElement("div");
		line.className = "hline";
		line.innerHTML = `<span id="compass" class="compass" hidden></span>`;
		h1.after(line);
		const power = $("power");
		if (power) line.appendChild(power);
		pollInternet();
		pollCompass();
	}
	pollPower();
});


function startPolling(render) {
	async function tick() {
		try {
			const res = await fetch("/cgi-bin/status", { cache: "no-store" });
			const data = await res.json();
			setOnline(true);
			render(data);
			$("updated").textContent = "Updated " + new Date().toLocaleTimeString();
		} catch (e) {
			setOnline(false);
		}
		setTimeout(tick, POLL_MS);
	}
	tick();
}
