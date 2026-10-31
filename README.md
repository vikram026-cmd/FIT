# QNX SDP 8.0 Patient Monitor — threaded CSV + Google Sheets

## Project files
- `src/patient_monitor.c` — complete application, one executable and five worker threads.
- `support/pm_config.h` — configuration reference.
- `esp32_firmware.txt` — ESP32 UART firmware reference.
- `google-app/code.gs` — Google Apps Script endpoint.
- `pass/secrets.example.h` — credential template only. The real secrets file is intentionally excluded from this ZIP.

## Required credentials
Copy `pass/secrets.example.h` to `pass/secrets.h` and insert the actual values:
```c
#ifndef IR_SECRETS_H
#define IR_SECRETS_H
#define GOOGLE_SCRIPT_TOKEN "YOUR_TOKEN"
#define GOOGLE_SCRIPT_URL "YOUR_WEB_APP_URL"
#endif
```
Do not commit `pass/secrets.h` or share it publicly. Since the uploaded ZIP contained a real credential file, rotate/revoke the old Google Script token if this ZIP was shared outside your trusted environment.

## Sensor meaning and relay logic
The ESP32 firmware sends:
`HR=<integer>,TEMP=<float>,HUM=<float>,NTC=<0-or-1>`

- `HR` is **heart rate in BPM**, not blood pressure.
- `NTC=0` means alert.
- `NTC=1` means normal.
- Relay ON when `NTC == 0 || heart_rate > 130`.
- Relay OFF otherwise.
- GPIO17 is active-HIGH; shutdown attempts to force it LOW.

## Thread layout
1. UART receiver — priority 60
2. Data processor — priority 55
3. Relay controller — priority 65
4. CSV storage — priority 30
5. Google Sheets uploader — priority 20

The relay worker is assigned the highest priority. Consumers are started before the UART producer, but scheduling priority is independent of creation order.

## Files on the Pi
- CSV: `/data/home/qnxuser/apps/patient_monitor.csv`
- Offline queue: `/data/home/qnxuser/apps/pending_upload.csv`

No SQL or SQLite is used.

## Build and deploy
Keep this project root structure, including `pass/secrets.h`, then run from the Momentics build environment:
```sh
make clean
make build
make deploy
```
`make all` builds and deploys. Deployment streams the executable over SSH using `cat > file`; it does not use SCP.

Run on the Pi:
```sh
cd /data/home/qnxuser/apps
./patient_monitor
```

## Runtime requirements and limitations
The QNX target must provide `curl`, `gpio-rp1`, `msix-rp1`, and `devc-serpl011-rpi5`. The QNX target toolchain must compile and link the program. Explicit FIFO scheduling can fail with `EPERM` if the process lacks scheduling privileges.

This is a prototype, not a certified medical device. `system()`/`curl` and command-line GPIO control are not deterministic hard-real-time operations. The program uses one process with multiple threads; it does not provide separate-process fault isolation.
