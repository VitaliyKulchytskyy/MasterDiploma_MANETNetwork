import subprocess, sys, time
from cdp import Browser

OUT = sys.argv[1]
URL = "http://localhost:8000/"
A4 = 1357                     # 960 * 1.414: one A4 page
b = Browser(960, A4)

def content_height():
    return b.js("Math.ceil((document.querySelector('footer') || document.body).getBoundingClientRect().bottom + scrollY)")

def page(path, name, height=None, wait=6, js=None, after=1.5):
    b.size(A4)
    b.go(URL + path, wait)
    b.size(min(height or content_height(), A4))
    time.sleep(0.8)
    if js:
        b.js(js)
        time.sleep(after)
    b.shot(f"{OUT}/{name}.png")
    print(name)

# Node: overview, internet and radio; then the Wi-Fi spectrum with a channel under the pointer
page("index.html", "Node", wait=7)
page("index.html", "Node - Wi-Fi spectrum, channel details", wait=7, after=1.0, js="""(() => {
  window.scrollTo(0, document.documentElement.scrollHeight);
  const cv = document.getElementById('spec-fall'), ch = specChannels(), i = ch.indexOf(11);
  const col = (cv.clientWidth - SPEC.left) / ch.length, r = cv.getBoundingClientRect();
  cv.dispatchEvent(new MouseEvent('mousemove', {clientX: r.left + SPEC.left + (i + 0.5) * col, clientY: r.top + 4, bubbles: true}));
  return 1; })()""")
page("index.html", "Node - changing the mesh channel", height=A4, wait=7, after=2.0,
     js="document.getElementById('chan-edit').click(); setTimeout(() => document.getElementById('chan-form').scrollIntoView({block: 'center'}), 700); 1")
page("index.html", "Node - editing the mesh IP",
     js="document.getElementById('ip-edit').click(); const i = document.getElementById('ip-input'); i.value = '10.10.0.5/24'; i.dispatchEvent(new Event('input')); window.scrollTo(0, 0)")
# a taller window for the pen-editor: measure after opening
page("mesh.html", "Mesh")
page("peers.html", "Peers - 1 peers table and map", height=A4, wait=8)
page("peers.html", "Peers - 2 link graph and alarm log", height=A4, wait=8, js="window.scrollTo(0, document.documentElement.scrollHeight)")
END = "const l = document.getElementById('log'); l.scrollTop = l.scrollHeight; "
# from the "Yesterday" separator on: yesterday's messages, then "Today"
DAY = "const l = document.getElementById('log'), d = l.querySelector('.day-sep'); l.scrollTop = d.offsetTop - l.offsetTop - 12; 1"
page("chat.html", "Chat", height=1150, js=DAY)
page("chat.html", "Chat - SOS confirmation", height=1150, js=END + "document.getElementById('quick-sos').click()")
page("erl.html", "ERL")
page("erl.html", "ERL - clear chat confirmation", js="document.getElementById('clear-chat').click()")
page("info.html", "Info")

# a fresh SOS: the alarm
subprocess.run([sys.executable, "chat_fill.py", "/tmp/claude-1000/demo-run/chat.db", "sos"], check=True)
page("peers.html", "Peers - 3 SOS alarm", height=A4, wait=8)
page("peers.html", "Peers - 4 SOS position shown on map", height=A4, wait=8,
     js="document.querySelector('#alarm .link-btn').click()", after=2.5)
b.size(720)
b.go(URL + "peers.html", 8)
b.js("document.getElementById('map-expand').click()")
time.sleep(3)
b.shot(f"{OUT}/Peers - 5 full screen map.png")
print("Peers - 5 full screen map")
b.close()
