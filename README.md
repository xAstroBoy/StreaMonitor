# StreaMonitor C++

[![Build & Release](https://github.com/xAstroBoy/StreaMonitor/actions/workflows/build.yml/badge.svg)](https://github.com/xAstroBoy/StreaMonitor/actions/workflows/build.yml)

A high-performance C++ application for monitoring and recording live streams from various websites. This is a complete rewrite of the original Python-based StreaMonitor, offering significantly better performance and resource efficiency.

## Download

Grab the latest release from the [Releases](https://github.com/xAstroBoy/StreaMonitor/releases) page:

| Platform | File | Notes |
|----------|------|-------|
| **Windows** | `StreaMonitor-Windows-x64.zip` | Standalone – just unzip and run |
| **Linux** | `StreaMonitor-Linux-x64.tar.gz` | Needs system FFmpeg + OpenSSL |

Each archive contains a single standalone executable:
- `StreaMonitor` / `StreaMonitor.exe` — launches the **GUI** by default
- Run with `--cli` for **headless mode** (interactive shell + web dashboard on port 5000)

## Features

- **Multi-site support**: Monitor streams from multiple platforms simultaneously
- **High performance**: Native C++ implementation with minimal resource usage
- **GUI & CLI modes**: Both graphical and command-line interfaces available
- **Proxy support**: Multiple proxy types (HTTP, HTTPS, SOCKS4, SOCKS4A, SOCKS5, SOCKS5H) with pool rotation
- **Automatic recording**: Starts recording when streams go live, stops when offline
- **Resolution selection**: Choose preferred quality up to maximum available
- **Cross-platform**: Builds on Windows, Linux, and macOS

## Supported Sites

| Site | Abbreviation | Notes |
|------|--------------|-------|
| StripChat | SC | Requires mouflon keys |
| StripChat VR | SCVR | VR streams, requires mouflon keys |
| Chaturbate | CB | |
| CamSoda | CS | |
| Flirt4Free | F4F | |
| DreamCam | DC | |
| DreamCam VR | DCVR | VR streams |
| Streamate | SM | Also PornHubLive, PepperCams, etc. |
| BongaCams | BC | |
| Cam4 | C4 | |
| Cams.com | CC | |
| MyFreeCams | MFC | |
| XLoveCam | XLC | |
| FanslyLive | FL | |
| Cherry.tv | CHTV | |
| Amateur.tv | ATV | |
| SexChat.hu | SCHU | |
| ManyVids | MV | |

## Requirements

### Build Requirements

- **CMake** 3.20 or higher
- **C++20** compatible compiler:
  - Windows: Visual Studio 2022 or MSVC 14.30+
  - Linux: GCC 11+ or Clang 13+
  - macOS: Apple Clang 14+ or GCC 11+
- **vcpkg** package manager (recommended)

### Runtime Requirements

- FFmpeg libraries (bundled on Windows)
- OpenSSL (bundled on Windows)
- libcurl (bundled on Windows)

## Building

### Windows (Recommended)

1. Install [vcpkg](https://github.com/microsoft/vcpkg):
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   ```

2. Build the project:
   ```powershell
   cd StreaMonitor-CPP
   python build.py
   ```

3. The executables will be packaged to `dist/` with a release archive (`.zip`).

### Linux

1. Install dependencies via vcpkg or system packages:
   
   **Using vcpkg** (recommended):
   ```bash
   git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh
   export VCPKG_ROOT=~/vcpkg
   cd StreaMonitor-CPP
   python3 build.py
   ```

   **Using system libraries** (Debian/Ubuntu):
   ```bash
   sudo apt install build-essential cmake ninja-build \
       libcurl4-openssl-dev libssl-dev nlohmann-json3-dev \
       libspdlog-dev libfmt-dev libavcodec-dev libavformat-dev \
       libavfilter-dev libswresample-dev libswscale-dev \
       libglfw3-dev libgl1-mesa-dev
   
   cd StreaMonitor-CPP
   python3 build.py --system-libs
   ```

2. The executable will be in `dist/` with a release archive (`.tar.gz`).

### macOS

1. Install dependencies:
   ```bash
   brew install cmake ninja vcpkg
   export VCPKG_ROOT=$(brew --prefix)/opt/vcpkg
   ```

2. Build:
   ```bash
   cd StreaMonitor-CPP
   python3 build.py
   ```

## Usage

### GUI Mode (default)

Launch the graphical interface:
```bash
./StreaMonitor        # Linux/macOS
StreaMonitor.exe      # Windows
```

The GUI provides:
- Dashboard with active recordings
- Settings panel for configuration
- Model management (add/remove/start/stop)
- Real-time log viewer
- Disk usage monitoring

### CLI Mode (--cli)

For headless/server operation:
```bash
./StreaMonitor --cli        # Linux/macOS
StreaMonitor.exe --cli      # Windows
```

The CLI runs an interactive shell and exposes a web interface (default port 5000).

### Web Interface

Access the web dashboard at `http://localhost:5000` (configurable port).

## Configuration

Configuration is stored in `app_config.json` (or via environment variables).

### Configuration File Example

```json
{
  "downloadDirectory": "./downloads",
  "maxResolution": 99999,
  "httpTimeoutSec": 30,
  "sleepOnError": 60,
  "sleepOnOffline": 120,
  "container": "mkv",
  "userAgent": "Mozilla/5.0 ...",
  "proxyEnabled": false,
  "proxies": [
    {
      "url": "http://proxy1.example.com:8080",
      "type": "http",
      "enabled": true,
      "rolling": false,
      "name": "Primary"
    },
    {
      "url": "socks5://proxy2.example.com:1080",
      "type": "socks5",
      "enabled": true,
      "rolling": true,
      "name": "Rotating SOCKS5"
    }
  ],
  "proxyMaxFailures": 5,
  "proxyDisableSec": 300,
  "proxyAutoDisable": true,
  "models": [
    {
      "username": "example_user",
      "site": "CB",
      "autoStart": true
    }
  ]
}
```

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `STRMNTR_DOWNLOAD_DIR` | Download directory | `./downloads` |
| `STRMNTR_MAX_RESOLUTION` | Maximum video resolution | `99999` |
| `STRMNTR_CONTAINER` | Container format (mkv/mp4/ts) | `mkv` |
| `STRMNTR_HTTP_TIMEOUT` | HTTP timeout in seconds | `30` |
| `STRMNTR_SLEEP_ON_ERROR` | Sleep after error (seconds) | `60` |
| `STRMNTR_SLEEP_ON_OFFLINE` | Sleep when offline (seconds) | `120` |
| `STRMNTR_PROXY_ENABLED` | Enable proxy | `false` |
| `STRMNTR_PROXY_URL` | Proxy URL | - |
| `STRMNTR_PROXY_TYPE` | Proxy type | `http` |

### Proxy Types

- `none` - No proxy
- `http` - HTTP proxy
- `https` - HTTPS proxy  
- `socks4` - SOCKS4 proxy
- `socks4a` - SOCKS4A proxy (DNS resolved by proxy)
- `socks5` - SOCKS5 proxy
- `socks5h` - SOCKS5 proxy with remote DNS resolution

### Rolling Proxies

For rotating/rolling proxies, set `"rolling": true` in the proxy entry. The proxy pool will automatically cycle through available proxies with health tracking - proxies that fail multiple times are temporarily disabled.

## StripChat Keys

For StripChat/XHamsterLive support, you need to provide mouflon decryption keys in `stripchat_mouflon_keys.json`:

```json
{
  "keys": [
    "key1...",
    "key2..."
  ]
}
```

## Status Codes

| Status | Description |
|--------|-------------|
| Idle | Not running, waiting to start |
| Checking | Checking if stream is live |
| Offline | Stream is offline |
| Recording | Actively recording |
| Rate Limited | Temporarily blocked by site |
| Connection Error | Network/connection failure |
| Error | General error |
| Not Running | Stopped |

## Docker

```dockerfile
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    libcurl4 libssl3 ffmpeg \
    && rm -rf /var/lib/apt/lists/*

COPY StreaMonitor /app/
COPY web/ /app/web/
WORKDIR /app

VOLUME ["/app/downloads", "/app/config"]

EXPOSE 5000

CMD ["./StreaMonitor", "--cli"]
```

## Development

### Project Structure

```
StreaMonitor-CPP/
├── CMakeLists.txt          # Build configuration
├── build.py                # Build script
├── vcpkg.json              # vcpkg dependencies
└── src/
    ├── config/             # Configuration handling
    ├── core/               # Core types and base classes
    │   ├── types.h/cpp     # Common types (Status, ContainerFormat, etc.)
    │   ├── site_plugin.h/cpp  # Base class for site plugins
    │   └── bot_manager.h/cpp  # Model/bot management
    ├── downloaders/        # Recording implementation
    │   └── hls_recorder.h/cpp
    ├── gui/                # ImGui GUI application
    ├── net/                # Network layer
    │   ├── http_client.h/cpp  # HTTP client wrapper
    │   ├── proxy_pool.h/cpp   # Proxy pool with health tracking
    │   └── m3u8_parser.h/cpp  # HLS playlist parser
    ├── sites/              # Site-specific plugins
    │   ├── stripchat.cpp
    │   ├── chaturbate.cpp
    │   └── ...
    └── web/                # Web server
```

### Adding a New Site

1. Create a new file in `src/sites/`
2. Inherit from `SitePlugin`
3. Implement required methods:
   - `getSiteName()` / `getSiteSlug()`
   - `getWebsiteUrl()` / `getApiUrl()`
   - `resolve()` - Check if live and get stream URL
4. Register the plugin in `bot_manager.cpp`

## License

This project is for educational purposes only. Please respect content creators' wishes regarding recording.

## Credits

- Original Python implementation: [StreaMonitor](https://github.com/...)
- Inspired by [Recordurbate](https://github.com/oliverjrose99/Recordurbate)
