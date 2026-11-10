"""
Simulator ESP32 hidroponik — untuk menguji dashboard tanpa hardware.

Memakai topik & format yang sama persis dengan firmware.
    pip install paho-mqtt
    python sim_esp32.py hd7p2kq9vm          # meniru mode WiFi
    python sim_esp32.py hd7p2kq9vm --lte    # meniru mode seluler 4G (A7670E)
Lalu buka dashboard dengan #hd7p2kq9vm.
"""
import json
import math
import random
import sys
import time

import paho.mqtt.client as mqtt

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
CODE = ARGS[0] if ARGS else "hd7p2kq9vm"
NET = "lte" if "--lte" in sys.argv else "wifi"
PUB_EVERY = 5 if NET == "lte" else 2      # sama dengan firmware masing-masing mode
HOST, PORT = "broker.emqx.io", 1883
BASE = f"aranus/{CODE}/hidro"

cfg = {"ph_min": 5.5, "ph_max": 8.5, "ph_off": 0.0, "hyst": 0.10}
st = {"relay": 0, "mode": "auto", "manual_since": 0.0, "on_s": 0.0}
t_start = time.time()
force = False


def sensors(t):
    """Nilai sensor sintetis yang bergerak pelan + noise."""
    ph = 6.6 + 1.4 * math.sin(t / 240) + random.uniform(-0.05, 0.05)
    if st["relay"]:
        ph += -0.4 if ph > 7 else 0.4  # pompa koreksi menggeser pH ke tengah
    hour = time.localtime(t).tm_hour + time.localtime(t).tm_min / 60
    day = max(0.0, math.sin((hour - 6) / 12 * math.pi))
    temp = 24 + 6 * day + random.uniform(-0.2, 0.2)
    hum = 80 - 20 * day + random.uniform(-1, 1)
    lux = 30000 * day + random.uniform(0, 50)
    volt = 2.5 - (ph - 7.0 - cfg["ph_off"]) * 0.18
    return {"ph": round(ph + cfg["ph_off"], 2), "v": round(volt, 3), "t": round(temp, 1),
            "h": round(hum, 1), "lux": round(lux, 1)}


def history(now):
    """Riwayat 24 jam sintetis, format sama dengan firmware."""
    cur = int(now // 3600)
    out = {"end": cur * 3600, "ph": [], "t": [], "h": [], "lux": []}
    for i in range(23, -1, -1):
        s = sensors((cur - i) * 3600 + 1800)
        out["ph"].append(s["ph"]); out["t"].append(s["t"])
        out["h"].append(round(s["h"])); out["lux"].append(round(s["lux"]))
    return out


def on_connect(c, userdata, flags, rc, props=None):
    print("Terhubung ke broker", rc)
    c.publish(BASE + "/status", "online", qos=1, retain=True)
    c.publish(BASE + "/config", json.dumps(cfg), retain=True)
    c.publish(BASE + "/history", json.dumps(history(time.time())), retain=True)
    c.subscribe(BASE + "/set")


def on_message(c, userdata, msg):
    global force
    m = msg.payload.decode().strip().lower()
    print("perintah:", m)
    key, _, val = m.partition("=")
    if key == "relay":
        st["mode"], st["manual_since"] = "manual", time.time()
        st["relay"] = {"on": 1, "off": 0}.get(val, 1 - st["relay"])
    elif key == "mode":
        st["mode"] = "manual" if val == "manual" else "auto"
        st["manual_since"] = time.time()
    elif key in cfg:
        f = float(val)
        if key == "ph_min" and 0 <= f < cfg["ph_max"]: cfg[key] = f
        if key == "ph_max" and cfg["ph_min"] < f <= 14: cfg[key] = f
        if key == "ph_off" and -3 <= f <= 3: cfg[key] = f
        if key == "hyst" and 0 <= f <= 1: cfg[key] = f
        c.publish(BASE + "/config", json.dumps(cfg), retain=True)
    elif key == "ping":
        c.publish(BASE + "/config", json.dumps(cfg), retain=True)
    force = True


cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-{CODE}-{random.randint(0, 9999)}")
cli.will_set(BASE + "/status", "offline", qos=1, retain=True)
cli.on_connect, cli.on_message = on_connect, on_message
cli.connect(HOST, PORT, keepalive=30)
cli.loop_start()
print(f"Simulator berjalan (mode {NET.upper()}). Topik: {BASE}/#   (Ctrl+C untuk berhenti)")

last_pub = last_hist = 0.0
prev = time.time()
try:
    while True:
        now = time.time()
        s = sensors(now)
        if st["relay"]:
            st["on_s"] += now - prev
        prev = now
        if st["mode"] == "manual" and now - st["manual_since"] > 1800:
            st["mode"] = "auto"
        if st["mode"] == "auto":
            if s["ph"] < cfg["ph_min"] or s["ph"] > cfg["ph_max"]:
                st["relay"] = 1
            elif cfg["ph_min"] + cfg["hyst"] <= s["ph"] <= cfg["ph_max"] - cfg["hyst"]:
                st["relay"] = 0
        if force or now - last_pub >= PUB_EVERY:
            force, last_pub = False, now
            tele = {**s, "relay": st["relay"], "mode": st["mode"], "on_s": int(st["on_s"]),
                    "rssi": random.randint(-70, -55), "net": NET, "up": int(now - t_start), "ts": int(now)}
            cli.publish(BASE + "/telemetry", json.dumps(tele), retain=True)
        if now - last_hist >= 60:
            last_hist = now
            cli.publish(BASE + "/history", json.dumps(history(now)), retain=True)
        time.sleep(0.2)
except KeyboardInterrupt:
    cli.publish(BASE + "/status", "offline", qos=1, retain=True).wait_for_publish(2)
    cli.loop_stop()
