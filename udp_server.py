import socket
import json
import time
import threading
from flask import Flask, request, jsonify
from werkzeug.serving import make_server

# ======================================
#            CONFIG
# ======================================

HTTP_PORT = 6001

UDP_RX_IP = "0.0.0.0"
UDP_RX_PORT = 5005          # auto -> server (telemetria)

UDP_TX_PORT = 5006          # server -> autá (broadcast listen port v ns-3 appke)

# Demo pravidlo
SPEED_THRESHOLD = 15.0      # m/s (príklad) - uprav si
SLOW_SPEED = 10.0            # m/s
SLOW_DURATION_MS = 8000     # ako dlho má auto držať override
COMMAND_COOLDOWN_S = 2.0    # aby server neposielal stále dokola

# Broadcast target:
# - "255.255.255.255" niekedy funguje len v bridged,
# - pri host-only často treba napr. "192.168.56.255" (podľa tvojej siete).
BROADCAST_IP = "255.255.255.255"


# ======================================
#     HTTP SERVER – prijme SUMO mapu
# ======================================

app = Flask(__name__)
map_received = False


@app.route("/upload_map", methods=["POST"])
def upload_map():
    global map_received

    xml_map = request.data.decode("utf-8")

    with open("received_map.net.xml", "w") as f:
        f.write(xml_map)

    print("✔ MAPA prijatá, uložená ako received_map.net.xml")
    map_received = True

    return jsonify({"status": "ok", "bytes": len(xml_map)})


class FlaskThread(threading.Thread):
    """Flask server bežiaci v separátnom vlákne, aby sme ho vedeli vypnúť."""

    def __init__(self):
        super().__init__(daemon=True)
        self.srv = make_server("0.0.0.0", HTTP_PORT, app)
        self.ctx = app.app_context()
        self.ctx.push()

    def run(self):
        print(f"[HTTP] Server beží na porte {HTTP_PORT}, čakám na MAPU...")
        self.srv.serve_forever()

    def shutdown(self):
        print("[HTTP] Server sa ukončuje...")
        self.srv.shutdown()


# ======================================
#   UDP TX – posielanie príkazov autám
# ======================================

class UdpCommandSender:
    def __init__(self, broadcast_ip: str, port: int):
        self.broadcast_ip = broadcast_ip
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # povol broadcast
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    def send_slow_command(self, target_id: str, speed: float, duration_ms: int):
        msg = {
            "targetId": target_id,
            "cmd": "SLOW",
            "speed": float(speed),
            "durationMs": int(duration_ms),
            "serverTs": int(time.time() * 1000)
        }
        payload = json.dumps(msg).encode("utf-8")
        self.sock.sendto(payload, (self.broadcast_ip, self.port))
        print(f"📣 [CMD] broadcast -> {self.broadcast_ip}:{self.port} | {msg}")


# ======================================
#   UDP RX – prijem telemetrie od áut
# ======================================

def start_udp_server():
    global map_received

    # RX socket (telemetria)
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind((UDP_RX_IP, UDP_RX_PORT))

    print(f"[UDP-RX] Server beží na {UDP_RX_IP}:{UDP_RX_PORT}, prijímam správy...\n")

    # TX sender (príkazy)
    tx = UdpCommandSender(BROADCAST_IP, UDP_TX_PORT)

    # stav pre cooldown (aby to nespamovalo)
    last_command_time = {}  # dict: vehicle_id -> last_sent_epoch_s

    while True:
        data, addr = rx.recvfrom(4096)
        recv_time_ms = time.time() * 1000

        try:
            msg = json.loads(data.decode("utf-8"))
        except Exception:
            print("⚠ CHYBA JSON")
            continue

        veh_id = msg.get("id", "UNKNOWN")
        send_time = msg.get("timestamp", recv_time_ms)
        latency = recv_time_ms - send_time

        speed = msg.get("speed", None)

        print(f"✔ RX od {addr} | id={veh_id} | latency={latency:.2f} ms | speed={speed} | msg={msg}")

        # ===== DEMO LOGIKA =====
        if speed is None:
            continue

        try:
            speed_val = float(speed)
        except Exception:
            continue

        if speed_val > SPEED_THRESHOLD:
            print(f"Jakoooooooooooooooooooooo")
            now_s = time.time()
            last_s = last_command_time.get(veh_id, 0.0)

            if (now_s - last_s) >= COMMAND_COOLDOWN_S:
                tx.send_slow_command(
                    target_id=veh_id,
                    speed=SLOW_SPEED,
                    duration_ms=SLOW_DURATION_MS
                )
                last_command_time[veh_id] = now_s


# ======================================
#                MAIN
# ======================================

if __name__ == "__main__":

    # 1) Spusti HTTP server
    flask_thread = FlaskThread()
    flask_thread.start()

    # 2) Čakaj, kým mapa nepríde
    print("[SERVER] Čakám na prijatie mapy...")
    while not map_received:
        time.sleep(0.1)

    # 3) Mapa prišla → vypni HTTP server
    flask_thread.shutdown()

    print("\n[SERVER] MAPA prijatá → prepínam na UDP režim.\n")

    # 4) Spusti UDP server (blokujúce)
    start_udp_server()

