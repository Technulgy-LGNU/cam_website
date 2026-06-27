# Spinnaker Camera Website

A self-contained C++ web app for viewing a FLIR/Teledyne Spinnaker GigE camera in a browser.

## Build

```bash
make
```

The build expects the Spinnaker SDK at `/opt/spinnaker`. Override it if needed:

```bash
make SPINNAKER_ROOT=/path/to/spinnaker
```

## Run

```bash
./build/cam_website --bind 0.0.0.0 --port 8080
```

Then open:

```text
http://localhost:8080
```

Useful options:

```text
--serial SERIAL     Use a specific camera serial number
--bind ADDRESS      Bind address, default 0.0.0.0
--port PORT         HTTP port, default 8080
--help              Show options
```

## Autostart With Systemd

Build the app first:

```bash
make
```

Install and enable the service:

```bash
sudo cp systemd/cam_website.service /etc/systemd/system/cam_website.service
sudo systemctl daemon-reload
sudo systemctl enable --now cam_website.service
```

Check logs:

```bash
journalctl -u cam_website.service -f
```

Stop or disable it:

```bash
sudo systemctl stop cam_website.service
sudo systemctl disable cam_website.service
```

## Notes

- The app uses Spinnaker directly through the native C++ SDK; no Python packages or web frameworks are required.
- The browser polls `/frame.bmp` for the newest frame, `/status.json` for camera state, and `/cameras.json` for hotplug discovery.
- The camera dropdown switches cameras through `/select?serial=SERIAL` without restarting the process.
- If no camera is found, a selected camera drops, or a selected camera is not currently reachable, the capture loop retries automatically.
- For GigE cameras, make sure the camera is reachable on the host network and configured with Spinnaker's GigE tools if it is on the wrong subnet.
- If acquisition fails with access/capability errors on Linux, run `make rtcap` once. This applies the same `CAP_SYS_NICE,CAP_SYS_RESOURCE,CAP_NET_RAW` capabilities used by Spinnaker's sample Makefiles.

## Troubleshooting Access Errors

If `/status.json` shows a camera but `last_error` contains:

```text
Unable to set "DeviceAccessStatus" to Read/Write
```

check the system state outside this app:

```bash
/opt/spinnaker/bin/Acquisition
```

If the official sample fails with the same error, the app is not the blocker. Common fixes are:

- Close SpinView or any other process using the camera.
- Make sure no other computer on the network has control access to the GigE camera.
- Power-cycle the camera or unplug/replug Ethernet to clear a stale control channel.
- Run `make rtcap` once if Linux capabilities have not been applied to this binary.
