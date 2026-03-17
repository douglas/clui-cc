# Troubleshooting

If setup fails, run this first:

```bash
npm run doctor
```

This checks your local environment and prints pass/fail status without changing your system.

## Install Fails with "gyp" or "make" Errors

Install Xcode Command Line Tools, then retry:

```bash
xcode-select --install
```

```bash
npm install
```

## Install Fails with `ModuleNotFoundError: No module named 'distutils'`

Python 3.12+ removed `distutils`. Install `setuptools`:

```bash
python3 -m pip install --upgrade pip setuptools
```

```bash
npm install
```

If that still fails, install Python 3.11 and point npm to it:

```bash
brew install python@3.11
```

```bash
npm config set python $(brew --prefix python@3.11)/bin/python3.11
```

```bash
npm install
```

To undo that Python override later:

```bash
npm config delete python
```

## Install Fails with `fatal error: 'functional' file not found`

C++ headers are missing/broken, usually due to Xcode CLT issues.

Check toolchain first:

```bash
xcode-select -p
```

```bash
xcrun --sdk macosx --show-sdk-path
```

If either command fails (or the error persists), reinstall CLT:

```bash
sudo rm -rf /Library/Developer/CommandLineTools
```

```bash
xcode-select --install
```

Then retry:

```bash
npm install
```

If CLT is installed but the error still appears on newer macOS versions, compile explicitly against the SDK include path:

```bash
SDK=$(xcrun --sdk macosx --show-sdk-path)
clang++ -std=c++17 -isysroot "$SDK" -I"$SDK/usr/include/c++/v1" -x c++ - -o /dev/null <<'EOF'
#include <functional>
int main() { return 0; }
EOF
```

## Install Fails on `node-pty`

`node-pty` is native and requires macOS toolchains. Confirm:

- macOS 13+
- Xcode CLT installed
- Python 3 with `setuptools`/`distutils` available

Then retry `npm install`.

## App Launches but No Claude Response

Verify Claude CLI is installed and authenticated:

```bash
claude --version
```

```bash
claude
```

## `Alt+Space` Does Not Toggle

Grant Accessibility permissions:

- System Settings -> Privacy & Security -> Accessibility

Fallback shortcut:

- `Cmd+Shift+K`

## Marketplace Shows "Failed to Load"

Expected when offline. Marketplace needs internet access; core app features continue to work.

## Window Is Invisible / No UI

Try:

- `Cmd+Shift+K`
- Confirm app is running from the menu bar tray

## Linux: Gray Window Covers Full Screen

The GTK4 shell renders a full-screen gray rectangle instead of the 1040x720 pill.

Cause: `gtk4-layer-shell` requires `gtk_widget_set_size_request()` to enforce sizing — `gtk_window_set_default_size()` alone is not reliable on all compositors.

Check the build is current:

```bash
npm run linux:build-shell
```

## Linux: WebKitGTK Fails to Load Page

Check console output for "Page load failed" messages (the shell logs load events). Verify the Vite dev server is running and bound to `127.0.0.1`:

```bash
npm run linux:dev-renderer
```

Then confirm it's reachable:

```bash
curl -s http://127.0.0.1:5173/ | head -5
```
