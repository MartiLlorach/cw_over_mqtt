
import time
import threading
import pygame
import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC = "martineitor/cw/0"

BEEP_FREQUENCY = 500
SAMPLE_RATE = 44100

# Generate a continuous sine-wave tone in memory
pygame.mixer.pre_init(
    frequency=SAMPLE_RATE,
    size=-16,
    channels=1,
    buffer=256,
)
pygame.init()
pygame.mixer.init()

import math
import array

samples = array.array(
    "h",
    (
        int(5000 * math.sin(
            2 * math.pi * BEEP_FREQUENCY * i / SAMPLE_RATE
        ))
        for i in range(SAMPLE_RATE)
    ),
)

tone = pygame.mixer.Sound(buffer=samples.tobytes())
tone_channel = None
audio_lock = threading.Lock()


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"Connected to {BROKER}")
        client.subscribe(TOPIC)
        print(f"Listening on {TOPIC}")
    else:
        print(f"MQTT connection failed: {reason_code}")


def on_message(client, userdata, msg):
    global tone_channel

    payload = msg.payload.decode("utf-8", errors="replace").strip()

    with audio_lock:
        if payload == "1":
            if tone_channel is None or not tone_channel.get_busy():
                tone_channel = tone.play(loops=-1)
                print("KEY DOWN - tone ON")

        elif payload == "0":
            if tone_channel is not None:
                tone_channel.stop()
                tone_channel = None
                print("KEY UP - tone OFF")

        else:
            print(f"Ignoring unexpected payload: {payload!r}")


def main():
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"cw-receiver-{int(time.time())}",
    )

    client.on_connect = on_connect
    client.on_message = on_message

    try:
        print(f"Connecting to {BROKER}:{PORT} ...")
        client.connect(BROKER, PORT, keepalive=60)
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nReceiver stopped.")
    finally:
        with audio_lock:
            if tone_channel is not None:
                tone_channel.stop()
        client.disconnect()
        pygame.quit()


if __name__ == "__main__":
    main()
