// Connect the offline control panel to KernelSU's asynchronous root shell bridge.
"use strict";

const control = "/system/bin/sh /data/adb/modules/clipsync/webui.sh";
const toggle = document.getElementById("sync-toggle");
const refresh = document.getElementById("refresh");
const notice = document.getElementById("notice");
const scan = document.getElementById("scan");
const addPeer = document.getElementById("add-peer-toggle");
const peerForm = document.getElementById("peer-form");
const peerAddress = document.getElementById("peer-address");
let addingPeer = false;
let currentState = "unknown";
let busy = false;
let scanning = false;
let callbackId = 0;

function exec(action) {
    return new Promise((resolve, reject) => {
        const callback = `clipsync_callback_${Date.now()}_${callbackId++}`;
        const timer = setTimeout(() => {
            delete window[callback];
            reject(new Error("响应超时，请重试"));
        }, 8000);
        window[callback] = (errno, stdout, stderr) => {
            clearTimeout(timer);
            delete window[callback];
            if (errno !== 0) {
                reject(new Error((stderr || "操作失败，请重试").trim()));
                return;
            }
            resolve(stdout);
        };
        try {
            window.ksu.exec(`${control} ${action}`, "{}", callback);
        } catch (error) {
            clearTimeout(timer);
            delete window[callback];
            reject(error);
        }
    });
}

function parseStatus(output) {
    const status = { interfaces: [] };
    for (const line of output.trim().split("\n")) {
        const [key, value, address] = line.split("\t");
        if (key === "interface" && value && address) status.interfaces.push({ name: value, address });
        if (["state", "pid", "version", "device", "android", "psk"].includes(key)) status[key] = value;
    }
    if (!["running", "paused", "stopped", "unknown"].includes(status.state)) {
        throw new Error("模块状态无效");
    }
    status.peers = parsePeers(output.split("\n").filter((line) => line.startsWith("peer\t")).join("\n"));
    return status;
}

function showNotice(message) {
    notice.textContent = message;
    notice.hidden = !message;
}

function setBusy(value) {
    busy = value;
    refresh.disabled = value || scanning;
    toggle.disabled = value || !["running", "paused"].includes(currentState);
    document.querySelector("main").setAttribute("aria-busy", String(value));
}

function renderStatus(status) {
    currentState = status.state;
    const labels = {
        running: "同步已开启",
        paused: "同步已暂停",
        stopped: "服务未运行",
        unknown: "状态暂不可用"
    };
    document.getElementById("status-card").dataset.state = currentState;
    document.getElementById("status-title").textContent = labels[currentState];
    toggle.checked = currentState === "running";
    for (const key of ["version", "device", "android", "pid"]) {
        document.getElementById(key).textContent = status[key] && status[key] !== "0" ? status[key] : "—";
    }
    document.getElementById("psk").textContent = status.psk === "configured" ? "已配置" : "未配置";
    const interfaces = document.getElementById("interfaces");
    interfaces.replaceChildren();
    for (const network of status.interfaces) {
        const row = document.createElement("div");
        row.className = "interface";
        for (const value of [network.name, network.address]) {
            const text = document.createElement("span");
            text.textContent = value;
            row.append(text);
        }
        interfaces.append(row);
    }
    interfaces.hidden = status.interfaces.length === 0;
    document.getElementById("network-detail").hidden = !interfaces.hidden;
    document.getElementById("network-detail").textContent = interfaces.hidden ? "未连接" : "";
    if (!scanning) renderPeers(status.peers);
}

function renderUnavailable() {
    currentState = "unknown";
    document.getElementById("status-card").dataset.state = "unavailable";
    document.getElementById("status-title").textContent = "无法读取状态";
    toggle.checked = false;
    for (const key of ["version", "device", "android", "pid", "psk"]) document.getElementById(key).textContent = "—";
    document.getElementById("interfaces").replaceChildren();
    document.getElementById("interfaces").hidden = true;
    document.getElementById("network-detail").textContent = "状态未确认";
    document.getElementById("network-detail").hidden = false;
    if (!scanning) renderPeers([]);
}

function validAddress(address) {
    return /^(\d{1,3}\.){3}\d{1,3}$/.test(address) && address.split(".").every((part) => Number(part) <= 255);
}

function parsePeers(output) {
    const peers = [];
    const nodes = new Set();
    const addresses = new Set();
    const lines = output.trim().split("\n");
    const live = (line) => ["connected", "key_mismatch", "bridge_error"].includes(line.split("\t")[5]);
    lines.sort((left, right) => Number(live(right)) - Number(live(left)));
    for (const line of lines) {
        if (!line) continue;
        const [type, node, name, address, state, connection = "discovered", extra] = line.split("\t");
        if (type !== "peer" || !/^[0-9a-f]{8}$/.test(node) || !validAddress(address) || !["running", "paused"].includes(state) || extra !== undefined || !["connected", "connecting", "offline", "key_mismatch", "bridge_error", "discovered"].includes(connection)) {
            throw new Error("扫描结果无效");
        }
        if ((node !== "00000000" && nodes.has(node)) || addresses.has(address) || peers.length >= 32) continue;
        if (node !== "00000000") nodes.add(node);
        addresses.add(address);
        peers.push({ name: name || "ClipSync", address, state, connection });
    }
    return peers;
}

function renderPeers(peers) {
    const list = document.getElementById("peers");
    list.replaceChildren();
    for (const peer of peers) {
        const row = document.createElement("div");
        row.className = "peer-row";
        const copy = document.createElement("div");
        copy.className = "peer-copy";
        const name = document.createElement("p");
        name.className = "peer-name";
        name.textContent = peer.name;
        const address = document.createElement("p");
        address.className = "peer-address";
        address.textContent = peer.address;
        copy.append(name, address);
        const state = document.createElement("span");
        state.className = "peer-state";
        state.dataset.state = peer.state;
        state.dataset.connection = peer.connection;
        const labels = { connected: peer.state === "paused" ? "已连接 · 暂停" : "已连接", connecting: "连接中", offline: "离线", key_mismatch: "密钥不一致", bridge_error: "写入失败", discovered: "已发现" };
        state.textContent = labels[peer.connection];
        row.append(copy, state);
        list.append(row);
    }
    list.hidden = peers.length === 0;
    document.getElementById("peer-empty").hidden = peers.length > 0;
    document.getElementById("peer-empty").textContent = "暂无设备";
}

async function scanDevices() {
    if (scanning) return;
    scanning = true;
    scan.disabled = true;
    refresh.disabled = true;
    document.getElementById("peer-card").setAttribute("aria-busy", "true");
    document.getElementById("peers").replaceChildren();
    document.getElementById("peers").hidden = true;
    const empty = document.getElementById("peer-empty");
    empty.hidden = false;
    empty.textContent = "扫描中…";
    try {
        renderPeers(parsePeers(await exec("scan")));
    } catch (error) {
        empty.textContent = error.message;
    } finally {
        scanning = false;
        scan.disabled = false;
        refresh.disabled = busy;
        document.getElementById("peer-card").setAttribute("aria-busy", "false");
    }
}

async function connectPeer(event) {
    event.preventDefault();
    if (addingPeer) return;
    const address = peerAddress.value.trim();
    if (!validAddress(address)) { showNotice("IP 地址无效"); return; }
    addingPeer = true;
    document.getElementById("connect-peer").disabled = true;
    showNotice("");
    try {
        await exec(`add-peer ${address}`);
        peerForm.hidden = true;
        peerAddress.value = "";
        await update();
    } catch (error) {
        showNotice(error.message);
    } finally {
        addingPeer = false;
        document.getElementById("connect-peer").disabled = false;
    }
}

function refreshAll() {
    update();
    scanDevices();
}

async function update(action = "status") {
    if (busy) return;
    setBusy(true);
    showNotice("");
    let actionError = "";
    try {
        if (action !== "status") {
            try { await exec(action); } catch (error) { actionError = error.message; }
        }
        renderStatus(parseStatus(await exec("status")));
        showNotice(actionError);
    } catch (error) {
        renderUnavailable();
        showNotice([actionError, error.message].filter(Boolean).join("\n"));
    } finally {
        setBusy(false);
    }
}

const theme = document.getElementById("theme");
try { theme.value = localStorage.getItem("clipsync-theme") || "auto"; } catch { theme.value = "auto"; }
if (!["auto", "light", "dark"].includes(theme.value)) theme.value = "auto";
function applyTheme() {
    if (theme.value === "auto") delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = theme.value;
}
applyTheme();
theme.addEventListener("change", () => {
    applyTheme();
    try { localStorage.setItem("clipsync-theme", theme.value); } catch { showNotice("无法保存外观设置"); }
});

if (window.ksu && typeof window.ksu.exec === "function") {
    addPeer.disabled = false;
    addPeer.addEventListener("click", () => { peerForm.hidden = !peerForm.hidden; if (!peerForm.hidden) peerAddress.focus(); });
    peerForm.addEventListener("submit", connectPeer);
    refresh.addEventListener("click", refreshAll);
    scan.addEventListener("click", scanDevices);
    toggle.addEventListener("change", () => {
        const action = toggle.checked ? "resume" : "pause";
        toggle.checked = currentState === "running";
        update(action);
    });
    document.addEventListener("visibilitychange", () => { if (!document.hidden) refreshAll(); });
    refreshAll();
    setInterval(() => { if (!document.hidden && !busy && !scanning && !addingPeer) update(); }, 3000);
} else {
    renderUnavailable();
    document.getElementById("status-title").textContent = "请在 KernelSU 中打开";
    refresh.disabled = true;
}
