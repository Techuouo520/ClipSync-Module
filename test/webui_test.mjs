// Offline regression tests for the WebUI bridge, status rendering, and shell controls.
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import vm from "node:vm";
import { test } from "node:test";

const root = resolve(import.meta.dirname, "..");
const app = readFileSync(join(root, "module/webroot/app.js"), "utf8");
const html = readFileSync(join(root, "module/webroot/index.html"), "utf8");

class Element {
    textContent = "";
    value = "";
    checked = false;
    disabled = false;
    hidden = false;
    dataset = {};
    children = [];
    listeners = {};
    addEventListener(event, listener) { this.listeners[event] = listener; }
    setAttribute() {}
    replaceChildren() { this.children = []; }
    append(...elements) { this.children.push(...elements); }
    focus() {}
}

function createPage({ connected = true, state = "running", failAction = false, noReply = false, peers = "", scanError = "", knownPeers = "" } = {}) {
    const elements = Object.fromEntries([...html.matchAll(/id="([^"]+)"/g)].map((match) => [match[1], new Element()]));
    const document = {
        documentElement: new Element(),
        getElementById(id) { assert.ok(elements[id], `Missing HTML element ${id}`); return elements[id]; },
        querySelector() { return new Element(); },
        createElement() { return new Element(); },
        addEventListener() {}
    };
    const calls = [];
    const timers = new Map();
    const context = { document, Date, Promise, console, localStorage: { getItem: () => null, setItem() {} } };
    context.setInterval = () => 0;
    context.setTimeout = (callback) => { const id = Symbol(); timers.set(id, callback); return id; };
    context.clearTimeout = (id) => timers.delete(id);
    context.window = context;
    if (connected) context.ksu = { exec(command, options, callback) {
        calls.push(command);
        assert.equal(options, "{}");
        if (noReply) return;
        const action = command.split(" ").at(-1);
        if (action === "scan") { context[callback](scanError ? 1 : 0, peers, scanError); return; }
        if (failAction && action !== "status") { context[callback](1, "", "control failed"); return; }
        if (action === "pause") state = "paused";
        if (action === "resume") state = "running";
        const output = `state\t${state}\npid\t42\nversion\tv0.2.0\ndevice\t<svg onload=alert(1)>\nandroid\t17\npsk\tconfigured\ninterface\twlan0\t192.0.2.2/24\n${knownPeers}`;
        context[callback](0, action === "status" ? output : state, "");
    } };
    vm.createContext(context);
    vm.runInContext(app, context);
    return { context, elements, calls, timers };
}

const settle = () => new Promise((resolve) => setImmediate(resolve));

test("running status, pause/resume, and text-only device rendering", async () => {
    const page = createPage();
    await settle();
    assert.equal(page.elements["status-title"].textContent, "同步已开启");
    assert.equal(page.elements.device.textContent, "<svg onload=alert(1)>");
    assert.equal(page.elements.interfaces.children.length, 1);
    assert.equal(page.elements.psk.textContent, "已配置");
    const toggle = page.elements["sync-toggle"];
    toggle.checked = false;
    toggle.listeners.change();
    assert.equal(toggle.disabled, true);
    assert.equal(toggle.checked, true);
    await settle();
    assert.equal(toggle.checked, false);
    assert.equal(page.elements["status-title"].textContent, "同步已暂停");
    toggle.checked = true;
    toggle.listeners.change();
    await settle();
    assert.equal(toggle.checked, true);
    assert.equal(page.calls.filter((command) => command.endsWith(" pause")).length, 1);
    assert.equal(page.timers.size, 0);
});

test("failed control re-reads actual state and reports the error", async () => {
    const page = createPage({ failAction: true });
    await settle();
    page.elements["sync-toggle"].checked = false;
    page.elements["sync-toggle"].listeners.change();
    await settle();
    assert.equal(page.elements["sync-toggle"].checked, true);
    assert.equal(page.elements.notice.textContent, "control failed");
});

test("stopped, unknown, and malformed states disable controls", async () => {
    for (const state of ["stopped", "unknown", "invalid"]) {
        const page = createPage({ state });
        await settle();
        assert.equal(page.elements["sync-toggle"].disabled, true);
        if (state === "invalid") assert.equal(page.elements.notice.hidden, false);
    }
});

test("bridge timeout cleans callbacks and permits refresh", async () => {
    const page = createPage({ noReply: true });
    for (const callback of page.timers.values()) callback();
    await settle();
    assert.equal(page.elements.refresh.disabled, false);
    assert.equal(page.elements["status-title"].textContent, "无法读取状态");
    assert.equal(Object.keys(page.context).some((key) => key.startsWith("clipsync_callback_")), false);
});

test("ordinary browser shows KernelSU guidance and theme selection works", () => {
    const page = createPage({ connected: false });
    assert.equal(page.elements.refresh.disabled, true);
    assert.equal(page.elements["status-title"].textContent, "请在 KernelSU 中打开");
    page.elements.theme.value = "dark";
    page.elements.theme.listeners.change();
    assert.equal(page.context.document.documentElement.dataset.theme, "dark");
});

test("scan lists real response fields, deduplicates peers, and renders names as text", async () => {
    const peer = "peer\t00000001\t<img src=x onerror=alert(1)>\t192.0.2.10\tpaused\tconnected\n";
    const page = createPage({ peers: peer + peer + "peer\t00000002\tPhone\t192.0.2.11\trunning\n" });
    await settle();
    assert.equal(page.elements.peers.children.length, 2);
    const [copy, state] = page.elements.peers.children[0].children;
    assert.equal(copy.children[0].textContent, "<img src=x onerror=alert(1)>");
    assert.equal(copy.children[1].textContent, "192.0.2.10");
    assert.equal(state.textContent, "已连接 · 暂停");
    assert.equal(page.elements["peer-empty"].hidden, true);
    page.elements.scan.listeners.click();
    await settle();
    assert.equal(page.elements.peers.children.length, 2);
});

test("scan empty, malformed, and error responses remain distinct", async () => {
    for (const [options, message] of [
        [{ peers: "" }, "暂无设备"],
        [{ peers: "peer\t00000001\tPhone\t999.0.2.1\trunning\n" }, "扫描结果无效"],
        [{ scanError: "Wi-Fi 未连接" }, "Wi-Fi 未连接"]
    ]) {
        const page = createPage(options);
        await settle();
        assert.equal(page.elements["peer-empty"].textContent, message);
        assert.equal(page.elements.scan.disabled, false);
        assert.equal(page.elements.peers.children.length, 0);
        assert.equal(page.elements["sync-toggle"].disabled, false);
    }
});

test("refresh clears previous scan results and prevents concurrent scans", async () => {
    const page = createPage({ peers: "peer\t00000001\tPhone\t192.0.2.10\trunning\n" });
    await settle();
    let pending;
    page.context.ksu.exec = (command, options, callback) => { pending = callback; };
    page.elements.scan.listeners.click();
    page.elements.scan.listeners.click();
    assert.equal(page.elements.peers.children.length, 0);
    assert.equal(page.elements["peer-empty"].textContent, "扫描中…");
    page.context[pending](0, "", "");
    await settle();
    assert.equal(page.elements["peer-empty"].textContent, "暂无设备");
});

test("known disconnected devices keep separate rows and connection status is explicit", async () => {
    const knownPeers = "peer\t00000000\tPhone A\t192.0.2.10\trunning\toffline\npeer\t00000000\tPhone B\t192.0.2.11\trunning\tconnecting\n";
    const page = createPage({ knownPeers });
    await settle();
    await page.context.update();
    assert.equal(page.elements.peers.children.length, 2);
    assert.equal(page.elements.peers.children[0].children[1].textContent, "离线");
    assert.equal(page.elements.peers.children[1].children[1].textContent, "连接中");
});

test("manual IP rejects shell input and sends only a validated address", async () => {
    const page = createPage();
    await settle();
    const event = { preventDefault() {} };
    page.elements["peer-address"].value = "192.0.2.1; id";
    await page.elements["peer-form"].listeners.submit(event);
    assert.equal(page.elements.notice.textContent, "IP 地址无效");
    assert.equal(page.calls.some((command) => command.includes(" add-peer ")), false);
    page.elements["peer-address"].value = "192.0.2.1";
    await page.elements["peer-form"].listeners.submit(event);
    assert.equal(page.calls.some((command) => command.endsWith(" add-peer 192.0.2.1")), true);
    assert.equal(page.elements["peer-form"].hidden, true);
});

test("a connected address takes precedence over stale device metadata", async () => {
    const peers = "peer\t00000001\tPhone\t192.0.2.10\trunning\toffline\npeer\t00000001\tPhone\t192.0.2.11\trunning\tconnected\n";
    const page = createPage({ peers });
    await settle();
    assert.equal(page.elements.peers.children.length, 1);
    assert.equal(page.elements.peers.children[0].children[0].children[1].textContent, "192.0.2.11");
    assert.equal(page.elements.peers.children[0].children[1].textContent, "已连接");
});

test("shell validates PID, makes controls idempotent, and handles failures", () => {
    const directory = mkdtempSync(join(tmpdir(), "clipsync-webui-test-"));
    const shellPath = (path) => path.replaceAll("\\", "/");
    const work = shellPath(directory);
    const script = readFileSync(join(root, "module/webui.sh"), "utf8")
        .replace("WORKDIR=/data/adb/clipsync", `WORKDIR='${work}'`)
        .replaceAll("/data/adb/ksu/bin/busybox", `${work}/busybox`)
        .replaceAll("/system/bin/sh", `'${shellPath(process.env.CLIPSYNC_BASH || "/bin/bash")}'`);
    const fixture = `
getprop() { printf 'test-device\\n'; }
ip() { printf '1: wlan0 inet 192.0.2.2/24 scope global wlan0\\n2: rmnet0 inet 192.0.2.3/24 scope global rmnet0\\n'; }
readlink() { printf '%s\\n' "\$FAKE_EXE"; }
sleep() { :; }
kill() {
    if [ "\$1" = -0 ]; then return "\$DEAD"; fi
    printf 'signal\\n' >> "\$WORKDIR/signals"
    if [ "\$SIGNAL_FAIL" = 1 ]; then return 1; fi
    if [ "\$STALL" = 1 ]; then return 0; fi
    if [ "$(cat "$WORKDIR/state")" = running ]; then printf 'paused\\n' > "\$WORKDIR/state"; else printf 'running\\n' > "\$WORKDIR/state"; fi
}
`;
    function run(action, overrides = {}) {
        return spawnSync(process.env.CLIPSYNC_BASH || "bash", [shellPath(join(directory, "webui.sh")), action], {
            encoding: "utf8",
            env: { ...process.env, BASH_ENV: `${work}/fixture.sh`, FAKE_EXE: "/system/bin/clipsyncd", DEAD: "0", SIGNAL_FAIL: "0", STALL: "0", LOCKED: "0", ...overrides }
        });
    }
    try {
        writeFileSync(join(directory, "webui.sh"), script);
        writeFileSync(join(directory, "module.prop"), "version=v0.1.2\n");
        writeFileSync(join(directory, "fixture.sh"), fixture);
        const busybox = '#!/usr/bin/env bash\nset -euo pipefail\nexec 9>&-\nif [[ "$3" =~ ^[0-9]+$ ]]; then echo "flock: Bad file descriptor" >&2; exit 1; fi\nif [ "$LOCKED" = 1 ]; then exit 1; fi\n: > "$3"\nshift 3\nexec "$@"\n';
        writeFileSync(join(directory, "busybox"), busybox, { mode: 0o755 });
        writeFileSync(join(directory, "clipsyncd.pid"), "42\n");
        writeFileSync(join(directory, "state"), "running\n");
        let result = run("status");
        assert.equal(result.status, 0, result.stderr || String(result.error));
        assert.match(result.stdout, /state\trunning/);
        assert.match(result.stdout, /interface\twlan0\t192.0.2.2\/24/);
        assert.doesNotMatch(result.stdout, /rmnet0/);
        assert.equal(run("pause").stdout.trim(), "paused");
        assert.equal(run("pause").stdout.trim(), "paused");
        assert.equal(readFileSync(join(directory, "signals"), "utf8"), "signal\n");
        assert.equal(run("resume").stdout.trim(), "running");
        assert.equal(run("toggle").stdout.trim(), "paused");
        assert.notEqual(run("resume", { STALL: "1" }).status, 0);
        assert.notEqual(run("resume", { SIGNAL_FAIL: "1" }).status, 0);
        assert.notEqual(run("resume", { LOCKED: "1" }).status, 0);
        assert.match(run("status", { DEAD: "1" }).stdout, /state\tstopped/);
        assert.match(run("status", { FAKE_EXE: "/system/bin/unrelated" }).stdout, /state\tstopped/);
        assert.notEqual(run("resume", { FAKE_EXE: "/system/bin/unrelated" }).status, 0);
        writeFileSync(join(directory, "peers.state"), "peer\t00000001\tPhone\t192.0.2.2\trunning\tconnected\n");
        assert.match(run("status", { DEAD: "1" }).stdout, /peer\t00000001\tPhone\t192.0.2.2\trunning\toffline/);
        for (const pid of ["0", "1", "-42", "garbage"]) {
            writeFileSync(join(directory, "clipsyncd.pid"), pid);
            assert.match(run("status").stdout, /state\tstopped/);
            assert.notEqual(run("pause").status, 0);
        }
        assert.equal(run("invalid").status, 2);
        assert.ok(existsSync(join(directory, "webui.lock")));
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
});
