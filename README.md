# url_preview — Profanity URL Preview Plugin

A native C plugin for [Profanity](https://profanity-im.github.io/) that automatically fetches and displays link previews in chat messages.

When someone sends a URL, the plugin parses the page and appends the **title** and **description** directly in the message display — no need to click through.

```
21:30 alice  hey check this out https://xmpp.org
                   🔗 Title: XMPP - The universal messaging standard
                   📝 Desc: XMPP is the Extensible Messaging and Presence Protocol…
```

## Features

- **Automatic preview** — works in private chats, MUCs, and MUC private messages
- **Smart filtering** — skips images, videos, archives, PDFs, and other non-HTML files by extension
- **Domain exclusion** — ignores same-JID domains, local/private IPs, and configurable domain blacklists
- **Adjustable timeout** — configurable HTTP timeout (default 2s)
- **Persistent cache** — caches results to disk; survives restarts
- **Configurable cache size** — automatic LRU-style eviction
- **Dependencies** - Zero dependencies beyond Profanity's plugin API

## Building

```bash
make
```

This produces `build/url_preview.so`.

## Installation

Install the plugin to your local Profanity plugins directory:

```bash
make install
```

This copies the plugin to `~/.local/share/profanity/plugins/`.

## Usage

Load the plugin in Profanity:

```
/plugins load url_preview.so
```

## Configuration

All settings are managed via the `/url_preview` command inside Profanity.

### Commands

| Command | Description |
|---|---|
| `/url_preview enable [on\|off]` | Enable or disable URL previews (default: **on**) |
| `/url_preview cache [on\|off]` | Enable or disable caching (default: **on**) |
| `/url_preview cache_size [N]` | Maximum number of cached previews (default: **1000**) |
| `/url_preview timeout [N]` | HTTP timeout in seconds (default: **2**) |
| `/url_preview ignored_extensions [list]` | Comma-separated file extensions to skip |
| `/url_preview ignored_domains [list]` | Comma-separated domains to skip |

### Defaults

| Setting | Default |
|---|---|
| `enable` | `on` |
| `cache` | `on` |
| `cache_size` | `1000` |
| `timeout` | `2` seconds |
| `ignored_domains` | `conversations.im` |
| `ignored_extensions` | `.jpg,.jpeg,.png,.gif,.webp,.bmp,.svg,.tiff,.mp4,.mkv,.webm,.avi,.mov,.flv,.mp3,.ogg,.wav,.flac,.m4a,.pdf,.doc,.docx,.xls,.xlsx,.ppt,.pptx,.zip,.rar,.7z,.tar,.gz,.bz2,.xz,.exe,.bin,.iso,.dmg,.apk` |

### Examples

```
/url_preview timeout 5
/url_preview ignored_domains upload.example.com,share.org
/url_preview ignored_extensions .jpg,.png,.gif,.pdf
/url_preview cache_size 500
/url_preview enable off
```

## How It Works

1. **Intercepts** incoming messages via `prof_pre_chat_message_display`, `prof_pre_room_message_display`, and `prof_pre_priv_message_display`.
2. **Extracts** HTTP/HTTPS URLs using a regex.
3. **Filters out** URLs that match:
   - The same domain (or a subdomain) as the sender's JID
   - Local/private IP addresses (`localhost`, `192.168.x.x`, `10.x.x.x`, `.local`, etc.)
   - Configured ignored domains
   - Configured ignored file extensions
4. **Checks the cache** for a previously stored result.
5. **Fetches** the URL with libcurl (up to 16 KB), validates the content type, and extracts:
   - `<title>` tag
   - `og:description` meta tag (preferred)
   - `<meta name="description">` as fallback
6. **Unescapes** HTML entities in the extracted text.
7. **Appends** the preview to the message and **caches** the result for future lookups.

Only the **first valid URL** in a message gets a preview.

## Cache

Previews are persisted to `~/.local/share/profanity/url_preview_cache.ini` in INI format. The cache is loaded at startup and trimmed to the configured `cache_size` when it overflows. Setting `cache_size` to a smaller value immediately evicts the oldest entries.

## Caveats

### Profanity UI freezes during URL fetch

Profanity's only provides synchronous, blocking hooks for message display (`prof_pre_chat_message_display`, etc.). There is **no asynchronous callback mechanism** — the plugin must return the modified message string before the message is rendered.

This means the entire Profanity UI **freezes** while the plugin fetches a URL. With the default 2-second timeout, that's up to 2 seconds where Profanity is completely unresponsive (typing, window switches, notifications — all blocked).

**Mitigation tips:**

- Set the timeout as low as you can tolerate: `/url_preview timeout 1`
- Disable the plugin when you don't need it: `/url_preview enable off`

## License

MIT
