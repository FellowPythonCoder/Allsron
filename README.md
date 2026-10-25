# Sreon

A fast, private browser for the Mac. Dark by design, built around a sidebar, a command bar and a built-in workshop.
**Powered by SPRFST Language.**

Sreon is written in SPRFST, a small language with its own compiler and virtual machine (`compiler/`, written in C).
Every decision and every page the browser shows, including the settings, the new tab page, the ad blocker, the command bar and Studio, is a SPRFST program (`engine/`).
macOS needs a native window and a web engine, so a thin Swift layer (`shell/`) provides the window, tabs, and Apple's WebKit, which draws real websites (all HTML, CSS and JavaScript). It does nothing else: it forwards every click to the engine and carries out what the engine answers.

## What is in it
- **Sidebar tabs** with pinned tabs, workspaces, sleeping tabs and a compact mode.
- **Command bar** (⌘L / ⌘K): search, addresses, your tabs, history, bookmarks, a calculator, unit conversion and 55 commands. `>` commands, `@` tabs, `#` bookmarks, keyword shortcuts (`g cats`).
- **Shield**: an ad, tracker, cookie-banner, pop-up and cryptominer blocker built on WebKit content rules (about 640 network rules and 400 cosmetic rules in `rules/shield.rules`), plus tracking-parameter cleaning, HTTPS upgrades, Global Privacy Control and YouTube ad skipping. Levels from Off to Maximum, per-site switches.
- **Brief, Reader, Notes per site, Focus mode and Pomodoro**, split view, screenshots, PDF, per-site zoom, dark mode for websites.
- **Studio** (⇧⌘S): code editor with SPRFST syntax colouring, Run / Check / Format with a real compiler, problems list, 21 examples, a 32-chapter **guidebook** with runnable code, a **whiteboard**, and 11 **tools** (JSON, Base64, URL, hash, colour, generators, text, time, Markdown, diff, calculator).
- **Settings** (⌘,): about 90 options: themes, accents, corner radius, glass blur, font, scale, density, sidebar side and width, search engine, shield levels, speed, startup, editor, and every keyboard shortcut.
- No Google branding anywhere. The default search engine is DuckDuckGo; Sreon adds no tracking of its own. Data lives in `~/.sreon` as plain JSON.

## What's new in 1.1

- **Celestial**: a calmer, minimal look. Deep-space default theme, hairline borders, one accent.
- **A desktop in the new tab**: status strip, clock, search and a dock with your tools and your own apps (open ones get a dot).
- **A boot sequence that behaves**: only the splash shows while Sreon starts, the browser window appears after it closes, and a watchdog removes the splash if start-up ever stalls.
- **The sidebar closes** (and comes back from the toolbar).
- **Linux and Windows**: a `.deb` for amd64 and arm64 (`make deb-all`) built on Electron with the same engine and interface; see `desktop/README.md`.
- **Programs can take input**: `io.ask_num`, `io.ask_int`, `io.ask_yes`, `io.read_all`, plus an **Input** box in Studio.
- **Apps**: Studio runs an `app` as a live, clickable window; `sprfst new app <name>` starts a desktop-app project; two new guidebook chapters (31 and 32).
- A new mark and icon.

## Build it (on a Mac)
1. Install the command line tools once: `xcode-select --install`
2. In this folder run:
   ```
   tools/build_app.sh --install      # builds a universal Sreon.app and copies it to /Applications
   ```
   Other options: `--dmg` (make `build/Sreon-<version>.dmg`), `--native` (build faster for this Mac only).
3. Without Xcode: `make test` runs the engine test-suite on any machine.

First launch of an ad-hoc signed app: right-click Sreon.app, choose Open.

## Publish it
- **Sign and notarise** (so it opens without warnings): with an Apple Developer ID,
  ```
  export SREON_SIGN_ID="Developer ID Application: Your Name (TEAMID)"
  export SREON_NOTARY_PROFILE=<notarytool keychain profile>
  tools/build_app.sh --dmg
  ```
- **GitHub release**: push this folder to a repository and push a tag such as `v1.0.0`. `.github/workflows/release.yml` builds on a Mac runner and attaches the `.dmg` to a release. Add the secrets `MACOS_CERT_P12`, `MACOS_CERT_PASSWORD` and `MACOS_SIGN_ID` to sign it.

## Layout
```
shell/Sources/   Swift: window, tabs, WebKit, engine bridge, menus
engine/          SPRFST: the brain and every page (state, router, omnibox, shield, studio...)
ui/              the CSS and small scripts WebKit needs; embedded into engine/assets.spf by tools/embed.spf
rules/           shield.rules, the blocker's rule book
guidebook/       32 chapters shown in Studio
examples/        programs shown in Studio
compiler/, std/  the SPRFST language: compiler, VM and standard library
tools/           build_app.sh, make_icons.py, embed.spf, test.sh
assets/logo/     the Sreon mark, icon set and .icns
```
After editing `ui/`, run `sprfst run tools/embed.spf` (the build script does it for you).

## Honest notes
- The Swift shell was written without access to a Mac, so it has not been compiled yet. The first `tools/build_app.sh` run on a Mac is where any small compiler complaints would appear; they will be one-line fixes. Everything on the engine side (about 5,000 lines of SPRFST) is tested here (`tools/test.sh`).
- WebKit does not report how many requests a content rule blocked, so Shield shows only what Sreon can really count: links cleaned, HTTPS upgrades, pop-ups blocked, tabs put to sleep.
- Chrome extensions are not supported (WebKit has its own extension model).

## What it is written in

- **SPRFST** – the whole engine: every page, settings, search, Shield logic, Studio, tests (`engine/`, `tools/embed.spf`).
- **C** – the SPRFST compiler and VM itself (`compiler/`).
- **Swift** – the thin native Mac shell: windows, WebKit views, menus, boot splash (`shell/Sources/`).
- **CSS + a little JavaScript** – required by WebKit to draw the interface (`ui/`, `assets/splash.html`), embedded into the engine at build time.
- Build and support: bash, Python (icon rendering), Markdown (guidebook), JSON/plist.

## Developer search
Settings › Search: pick a developer engine (Stack Overflow, GitHub code, MDN, DevDocs, Sourcegraph, grep.app or your own `%s` address) and a keyword (default `d`). Built-in shortcuts: `gh`, `so`, `mdn`, `npm`, `crates`, `pypi`, `docsrs`, `go`, `mvn`, `hn`, `caniuse`, `rfc` (also as `!npm lodash`).
