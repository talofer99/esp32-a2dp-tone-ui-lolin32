# ESP32 A2DP Tone Tester for WEMOS LOLIN32

This is a Phase 1 proof-of-concept project for your Bluetooth-audio-transmitter idea.

What it does:
- creates a Wi-Fi access point
- serves a tiny web UI
- lets you enter the exact Bluetooth headset name
- starts the ESP32 as an A2DP source
- plays a built-in sine-wave test tone after the headset connects

What it does **not** do yet:
- no RCA / analog input yet
- no device scan list yet
- no reconnect manager yet
- changing to a different headset is easiest by rebooting

## Board
This project is configured for:
- `board = lolin32`

## Build / Upload
Open the folder in VS Code with PlatformIO and use the `lolin32` environment.

## Wi-Fi UI
After boot:
1. Join Wi-Fi network: `ESP32-Audio-Setup`
2. Password: `esp32audio`
3. Open: `http://192.168.4.1`
4. Put your headset in pairing mode
5. Enter the exact Bluetooth device name
6. Click **Connect**
7. Click **Play tone** once connected

## Notes
- This requires an original ESP32 board with Bluetooth Classic support.
- Your earlier ESP32-S3 idea would not be suitable for A2DP Classic output.
- The A2DP library version is pinned in `platformio.ini` for stability.

## Next Step
Once this works, the next iteration is to replace the tone generator with an analog capture pipeline.
