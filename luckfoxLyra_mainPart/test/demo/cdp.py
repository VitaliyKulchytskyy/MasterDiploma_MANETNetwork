"""Headless Chrome over CDP: light theme, desktop window, screenshots."""
import base64, json, subprocess, time, urllib.request, websocket

class Browser:
    def __init__(self, width, height, scale=2, port=9340):
        self.p = subprocess.Popen(["google-chrome", "--headless=new", f"--remote-debugging-port={port}", "--remote-allow-origins=*",
                                   "--user-data-dir=/tmp/claude-1000/cdp-demo", "--hide-scrollbars", "--lang=en-GB", "about:blank"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json")); break
            except Exception:
                time.sleep(.2)
        self.ws = websocket.create_connection([t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"])
        self.n = 0
        self.width, self.scale = width, scale
        self.size(height)
        self.call("Emulation.setEmulatedMedia", features=[{"name": "prefers-color-scheme", "value": "light"}])

    def call(self, m, **params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": m, "params": params}))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.n:
                return r

    def size(self, height):
        self.call("Emulation.setDeviceMetricsOverride", width=self.width, height=int(height), deviceScaleFactor=self.scale, mobile=False)

    def js(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
        return r["result"]["result"].get("value")

    def go(self, url, wait=6):
        self.call("Page.navigate", url=url)
        time.sleep(wait)

    def shot(self, path):
        r = self.call("Page.captureScreenshot", format="png")
        open(path, "wb").write(base64.b64decode(r["result"]["data"]))

    def close(self):
        self.p.kill()
