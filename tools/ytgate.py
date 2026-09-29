#!/usr/bin/env python3
"""hawkOS YouTube gateway.

YouTube cannot be used directly by a browser without a JavaScript engine: its
pages are assembled by script, and its video (VP9 / AV1 / H.264 in fragmented
MP4) is reached through signed URLs that only YouTube's own player code can
compute. This gateway runs on the host and does those two things for hawkOS:

  * pages   - it asks YouTube for search results and video details through
              yt-dlp and answers with plain server-rendered HTML the hawkOS
              browser can lay out;
  * video   - it downloads the video and audio streams and transcodes them on
              the fly with ffmpeg into an MPEG-1 / MP2 program stream, which
              the hawkOS kernel decodes itself (src/video.c, PL_MPEG);
  * images  - it relays thumbnails from i.ytimg.com, so the guest fetches them
              over plain HTTP instead of paying for a TLS handshake per image.

The hawkOS browser sends every request for a YouTube host here (see the
YT_GATEWAY note in src/app_browser.c). Under QEMU's user-mode network the host
is 10.0.2.2, which QEMU maps to this machine's loopback -- so the gateway only
listens on 127.0.0.1 by default.

Usage:
    python3 tools/ytgate.py [--port 8090] [--fps 24]

Needs yt-dlp and ffmpeg on PATH (~/.local/bin is added automatically), and a
JavaScript runtime such as deno for yt-dlp's YouTube support.

Routes:
    /                         home page
    /results?search_query=Q   search results
    /watch?v=ID               watch page, with a <video> the browser plays
    /stream/ID.mpg?q=Q&t=T    the video, as MPEG-1 video + MP2 audio (MPEG-PS):
                              Q is 144, 240, 360, 480 or 720, T a start time in
                              seconds. The response says where it really starts
                              (X-Start, snapped to a segment) and how long the
                              video is (X-Duration).
    /ytimg/<path>             relay of https://i.ytimg.com/<path>
    /shorts/ID, /embed/ID     redirect to /watch?v=ID
"""

import argparse
import html
import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

os.environ["PATH"] = os.path.expanduser("~/.local/bin") + os.pathsep + os.environ.get("PATH", "")

ARGS = None

# What each quality means on the wire: picture size and MPEG-1 bitrate (target
# and ceiling). MPEG-1 needs roughly twice the bits of H.264 for the same
# picture, and the guest decodes it in software, so the top step is 720p --
# which an emulated CPU without KVM still decodes at full frame rate.
QUALITIES = {
    144: (256, 144, "300k", "500k"),
    240: (426, 240, "550k", "900k"),
    360: (640, 360, "950k", "1500k"),
    480: (854, 480, "1500k", "2300k"),
    720: (1280, 720, "3000k", "4500k"),
}
DEFAULT_Q = 480
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/126.0 Safari/537.36")
INFO_TTL = 3 * 3600          # stream URLs YouTube hands out expire after ~6 h
SEARCH_TTL = 30 * 60
HOME_QUERY = "popular music videos"
HOME_TOPICS = ["music", "science", "gaming", "comedy", "news", "cooking", "space", "retro computers"]

_lock = threading.Lock()
_info_cache = {}             # id -> (time, info)
_search_cache = {}           # query -> (time, entries)
_thumb_cache = {}            # path -> bytes
_thumb_bytes = 0
THUMB_BUDGET = 64 << 20


def log(*a):
    sys.stderr.write(time.strftime("[%H:%M:%S] ") + " ".join(str(x) for x in a) + "\n")
    sys.stderr.flush()


# ------------------------------------------------------------------ yt-dlp

def ytdlp_json(args, timeout=120):
    cmd = ["yt-dlp", "-J", "--no-warnings", "--ignore-no-formats-error"] + args
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0 or not p.stdout.strip():
        err = (p.stderr or "").strip().splitlines()
        raise RuntimeError(err[-1] if err else "yt-dlp failed (exit %d)" % p.returncode)
    return json.loads(p.stdout)


def search(query, n=16):
    now = time.time()
    with _lock:
        hit = _search_cache.get(query)
        if hit and now - hit[0] < SEARCH_TTL:
            return hit[1]
    t0 = time.time()
    try:
        out = search_web(query, n)
    except Exception as e:
        log("web search failed (%s), falling back to yt-dlp" % e)
        out = []
    if out:
        log("search %r: %d results in %.1fs" % (query, len(out), time.time() - t0))
        with _lock:
            _search_cache[query] = (now, out)
        return out

    d = ytdlp_json(["--flat-playlist", "ytsearch%d:%s" % (n, query)])
    out = []
    for e in d.get("entries") or []:
        vid = e.get("id")
        # Channels and playlists come back from a search too; only videos
        # have an 11-character id and a watch URL.
        if not vid or len(vid) != 11 or "/watch?v=" not in (e.get("url") or "https://www.youtube.com/watch?v=" + vid):
            continue
        out.append({
            "id": vid,
            "title": e.get("title") or vid,
            "channel": e.get("channel") or e.get("uploader") or "",
            "duration": e.get("duration"),
            "views": e.get("view_count"),
            "live": e.get("live_status") == "is_live",
        })
    log("search %r: %d results in %.1fs" % (query, len(out), time.time() - t0))
    with _lock:
        _search_cache[query] = (now, out)
    return out


def _text(node):
    """YouTube's text objects are either {"simpleText": ...} or {"runs": [...]}."""
    if not isinstance(node, dict):
        return ""
    if "simpleText" in node:
        return node["simpleText"]
    return "".join(r.get("text", "") for r in node.get("runs") or [])


def _seconds(clock):
    try:
        n = 0
        for part in clock.split(":"):
            n = n * 60 + int(part)
        return n
    except ValueError:
        return None


def search_web(query, n):
    """Search by reading the results page YouTube serves to a browser. The
    page carries its data as a JSON literal (ytInitialData), which is one
    request and a parse -- a second or two, where yt-dlp's search takes ten to
    twenty. yt-dlp stays as the fallback for when the page changes shape."""
    url = ("https://www.youtube.com/results?search_query=%s&hl=en&gl=US"
           % urllib.parse.quote_plus(query))
    req = urllib.request.Request(url, headers={
        "User-Agent": UA, "Accept-Language": "en-US,en;q=0.9",
        "Cookie": "CONSENT=YES+1; SOCS=CAI"})
    page = urllib.request.urlopen(req, timeout=15).read().decode("utf-8", "replace")
    m = re.search(r"(?:var ytInitialData|window\[\"ytInitialData\"\])\s*=\s*(\{.+?\});\s*</script>", page, re.S)
    if not m:
        raise RuntimeError("no ytInitialData on the results page")
    data = json.loads(m.group(1))

    found = []

    def walk(node):
        if len(found) >= n:
            return
        if isinstance(node, dict):
            vr = node.get("videoRenderer")
            if isinstance(vr, dict) and len(vr.get("videoId") or "") == 11:
                badges = json.dumps(vr.get("badges") or []) + json.dumps(vr.get("thumbnailOverlays") or [])
                views = re.sub(r"[^0-9]", "", _text(vr.get("viewCountText")))
                found.append({
                    "id": vr["videoId"],
                    "title": _text(vr.get("title")) or vr["videoId"],
                    "channel": _text(vr.get("ownerText")) or _text(vr.get("longBylineText")),
                    "duration": _seconds(_text(vr.get("lengthText"))) if vr.get("lengthText") else None,
                    "views": int(views) if views else None,
                    "live": "BADGE_STYLE_TYPE_LIVE_NOW" in badges or '"LIVE"' in badges,
                })
                return
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    return found


_inflight = {}          # vid -> Event set when its extraction finishes


def video_info(vid, fresh=False):
    """yt-dlp's full extraction for a video: formats, URLs, description. It
    takes anywhere from two to fifteen seconds, so a request that arrives
    while one is already running for the same video waits for that one
    rather than starting a second."""
    now = time.time()
    with _lock:
        hit = _info_cache.get(vid)
        if hit and now - hit[0] < INFO_TTL and not fresh:
            return hit[1]
        ev = None if fresh else _inflight.get(vid)
        if ev is None:
            ev = threading.Event()
            _inflight[vid] = ev
            owner = True
        else:
            owner = False
    if not owner:
        ev.wait(120)
        with _lock:
            hit = _info_cache.get(vid)
        if hit:
            return hit[1]
        return video_info(vid, fresh=True)
    try:
        t0 = time.time()
        info = ytdlp_json(["--no-playlist", "https://www.youtube.com/watch?v=" + vid])
        log("info %s: %r in %.1fs" % (vid, info.get("title"), time.time() - t0))
        with _lock:
            _info_cache[vid] = (time.time(), info)
        return info
    finally:
        with _lock:
            if _inflight.get(vid) is ev:
                del _inflight[vid]
        ev.set()


def cached_info(vid):
    with _lock:
        hit = _info_cache.get(vid)
    return hit[1] if hit and time.time() - hit[0] < INFO_TTL else None


def prefetch_info(vid):
    """Starts extraction in the background, so the stream request that
    follows a watch page finds it done or under way."""
    def run():
        try:
            video_info(vid)
        except Exception as e:
            log("prefetch %s failed: %s" % (vid, e))
    threading.Thread(target=run, daemon=True).start()


def search_entry(vid):
    with _lock:
        for _, pool in _search_cache.values():
            for e in pool:
                if e["id"] == vid:
                    return e
    return None


def pick_formats(info, max_h):
    # MP4 (H.264 video, AAC audio) before WebM: YouTube's MP4 streams carry a
    # segment index, which is what makes seeking cheap -- see dash_index.
    """Returns (video_format, audio_format); either may be None, and when the
    best choice is a single muxed format it is returned as the video with
    audio None."""
    fm = [f for f in info.get("formats") or [] if f.get("url") and f.get("protocol") in ("https", "http")]

    def is_v(f): return f.get("vcodec") not in (None, "none")
    def is_a(f): return f.get("acodec") not in (None, "none")

    # H.264 first: it is by far the cheapest of YouTube's codecs for ffmpeg to
    # decode, and the source is being thrown away at 320 pixels anyway.
    def vkey(f):
        return ((f.get("vcodec") or "").startswith("avc1"), f.get("ext") == "mp4",
                f.get("height") or 0, f.get("tbr") or 0)

    muxed = [f for f in fm if is_v(f) and is_a(f) and (f.get("height") or 0) <= max_h]
    vonly = [f for f in fm if is_v(f) and not is_a(f) and (f.get("height") or 0) <= max_h]
    aonly = [f for f in fm if is_a(f) and not is_v(f)]
    aonly.sort(key=lambda f: (f.get("ext") == "m4a", f.get("abr") or 0))

    if vonly and aonly:
        vonly.sort(key=vkey)
        return vonly[-1], aonly[-1]
    if muxed:
        muxed.sort(key=vkey)
        return muxed[-1], None
    if vonly:
        vonly.sort(key=vkey)
        return vonly[-1], None
    return None, None


# --------------------------------------------------------------- streaming

CHUNK = 4 << 20   # ranged requests, the way YouTube's own player fetches

_index_cache = {}


def fetch_range(fmt, start, end):
    h = dict(fmt.get("http_headers") or {})
    h["Range"] = "bytes=%d-%d" % (start, end)
    return urllib.request.urlopen(urllib.request.Request(fmt["url"], headers=h), timeout=20).read()


def dash_index(fmt):
    """For a fragmented MP4 stream: (init bytes, [(start_seconds, offset)]).

    YouTube's MP4 streams open with ftyp and moov (the init segment) followed
    by a sidx box listing every media segment's byte size and duration. With
    that, starting at 5:00 is: send the init segment, then fetch from the byte
    where the segment containing 5:00 begins -- instead of downloading and
    decoding five minutes of video to throw it away. None for anything else."""
    key = fmt["url"]
    with _lock:
        if key in _index_cache:
            return _index_cache[key]
    result = None
    try:
        head = fetch_range(fmt, 0, (256 << 10) - 1)
        pos, init_end, sidx = 0, None, None
        while pos + 8 <= len(head):
            size = int.from_bytes(head[pos:pos + 4], "big")
            kind = head[pos + 4:pos + 8]
            hdr = 8
            if size == 1:
                size = int.from_bytes(head[pos + 8:pos + 16], "big")
                hdr = 16
            if size < hdr:
                break
            if kind == b"sidx":
                init_end = pos if init_end is None else init_end
                sidx = (pos, size, hdr)
                break
            if kind in (b"moof", b"mdat"):
                break
            pos += size
        if sidx and sidx[0] + sidx[1] <= len(head):
            at, size, hdr = sidx
            b = head[at + hdr:at + size]
            version = b[0]
            timescale = int.from_bytes(b[8:12], "big")
            if version == 0:
                first = int.from_bytes(b[16:20], "big")
                q = 20
            else:
                first = int.from_bytes(b[20:28], "big")
                q = 28
            count = int.from_bytes(b[q + 2:q + 4], "big")
            q += 4
            offset = at + size + first
            t = 0
            segs = []
            for _ in range(count):
                ref = int.from_bytes(b[q:q + 4], "big") & 0x7FFFFFFF
                dur = int.from_bytes(b[q + 4:q + 8], "big")
                segs.append((t / timescale, offset))
                offset += ref
                t += dur
                q += 12
            if segs:
                result = (head[:init_end], segs)
    except Exception as e:
        log("no index for %s: %s" % (fmt.get("format_id"), e))
    with _lock:
        _index_cache[key] = result
    return result


def reachable(fmt):
    """Whether YouTube will actually serve this format's URL. It sometimes
    refuses one that yt-dlp has just handed out (403) -- an expired or
    token-bound URL -- and finding that out before ffmpeg starts is what lets
    the stream recover instead of arriving empty."""
    try:
        fetch_range(fmt, 0, 1023)
        return True
    except urllib.error.HTTPError as e:
        log("format %s refused: HTTP %d" % (fmt.get("format_id"), e.code))
        return False
    except Exception as e:
        log("format %s unreachable: %s" % (fmt.get("format_id"), e))
        return False


def pump_ytdlp(vid, fmt, path, stop):
    """Last resort: let yt-dlp download the format itself into the FIFO. It
    knows how to satisfy YouTube's per-client token rules; the cost is that
    there is no byte-range seeking, so a start time is reached by decoding."""
    try:
        with open(path, "wb", buffering=0) as out:
            proc = subprocess.Popen(
                ["yt-dlp", "-q", "--no-warnings", "--no-part", "-f", str(fmt["format_id"]),
                 "-o", "-", "https://www.youtube.com/watch?v=" + vid],
                stdout=out, stderr=subprocess.DEVNULL)
            while proc.poll() is None:
                if stop.wait(0.2):
                    proc.kill()
                    break
            proc.wait()
    except OSError as e:
        if not stop.is_set():
            log("yt-dlp pump %s ended: %s" % (fmt.get("format_id"), e))


def seek_point(index, t):
    """The last segment starting at or before t: (start_seconds, byte_offset)."""
    best = index[1][0]
    for seg in index[1]:
        if seg[0] <= t + 0.01:
            best = seg
        else:
            break
    return best


def pump(fmt, path, stop, start=0, prefix=b"", refresh=None):
    """Downloads one format from byte `start` and writes it, after `prefix`,
    into the FIFO at `path`. ffmpeg's static builds cannot resolve host names,
    so every byte from YouTube comes through Python and ffmpeg only ever reads
    local pipes.

    YouTube refuses the odd request (403) and drops the odd connection, even
    on a URL it served a moment before. A failed chunk is retried from where
    it stopped; from the second failure in a row, `refresh` supplies a freshly
    extracted URL for the same format. Only a write failing -- ffmpeg gone,
    because the viewer left -- ends the download quietly."""
    url = fmt["url"]
    headers = dict(fmt.get("http_headers") or {})
    size = fmt.get("filesize") or 0
    fid = fmt.get("format_id")
    try:
        out = open(path, "wb", buffering=0)
    except OSError:
        return
    try:
        if prefix:
            out.write(prefix)
        pos = start
        failures = 0
        while not stop.is_set():
            h = dict(headers)
            h["Range"] = "bytes=%d-%d" % (pos, pos + CHUNK - 1)
            got, ranged = 0, False
            try:
                r = urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=30)
                ranged = r.status == 206
                while not stop.is_set():
                    b = r.read(64 << 10)
                    if not b:
                        break
                    try:
                        out.write(b)
                    except OSError:
                        return                  # the reader is gone
                    got += len(b)
                    pos += len(b)
                r.close()
            except urllib.error.HTTPError as e:
                if e.code == 416:
                    break
                failures += 1
                log("pump %s: HTTP %d at byte %d (attempt %d)" % (fid, e.code, pos, failures))
            except (urllib.error.URLError, OSError) as e:
                failures += 1
                log("pump %s: %s at byte %d (attempt %d)" % (fid, e, pos, failures))
            else:
                if got:
                    failures = 0
                # The end: nothing came, the server ignored the range and sent
                # it all, the known size is reached, or (size unknown) a chunk
                # came back short.
                if got == 0 or not ranged or (size and pos >= size) or (not size and got < CHUNK):
                    break
                continue
            if failures > 6:
                log("pump %s: giving up" % fid)
                break
            if failures >= 2 and refresh:
                fresh = refresh(fid)
                if fresh:
                    url = fresh["url"]
                    headers = dict(fresh.get("http_headers") or {})
            if stop.wait(0.4 * failures):
                break
    finally:
        try:
            out.close()
        except OSError:
            pass


def ffmpeg_cmd(vpath, apath, q, fps, vskip=0.0, askip=0.0):
    w, h, rate, peak = QUALITIES[q]
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin"]
    # A skip given before an input discards that much of it by decoding: the
    # fallback for streams with no index, and the small trim that lines the
    # audio up with a video segment that starts a little later than its own.
    if vskip > 0:
        cmd += ["-ss", "%.3f" % vskip]
    cmd += ["-i", vpath]
    if apath:
        if askip > 0:
            cmd += ["-ss", "%.3f" % askip]
        cmd += ["-i", apath, "-map", "0:v:0", "-map", "1:a:0"]
    else:
        cmd += ["-map", "0:v:0", "-map", "0:a:0?"]
    cmd += [
        "-vf", "scale=%d:%d:force_original_aspect_ratio=decrease,"
               "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d" % (w, h, w, h, fps),
        # No B-frames: they buy little at these sizes and cost the guest a
        # reordering delay.
        "-c:v", "mpeg1video", "-b:v", rate, "-maxrate", peak, "-bufsize", peak,
        "-g", str(fps * 2), "-bf", "0",
        "-c:a", "mp2", "-ar", "44100", "-ac", "2", "-b:a", "128k",
        "-f", "mpeg", "pipe:1",
    ]
    return cmd


# ------------------------------------------------------------------- pages

def esc(s):
    return html.escape(str(s or ""), quote=True)


def fmt_views(n):
    if not n:
        return ""
    if n >= 1_000_000_000:
        return "%.1fB views" % (n / 1e9)
    if n >= 1_000_000:
        return "%.1fM views" % (n / 1e6)
    if n >= 1_000:
        return "%.0fK views" % (n / 1e3)
    return "%d views" % n


def fmt_dur(s):
    if not s:
        return ""
    s = int(s)
    if s >= 3600:
        return "%d:%02d:%02d" % (s // 3600, (s // 60) % 60, s % 60)
    return "%d:%02d" % (s // 60, s % 60)


STYLE = """<style>
h1 { color: #e62117; }
.meta { color: #707070; }
</style>"""


def page(title, body):
    return ("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>%s</head><body>"
            "<form action=\"/results\"><a href=\"/\"><b>YouTube</b></a> "
            "<input type=\"search\" name=\"search_query\" size=\"44\" placeholder=\"Search\"> "
            "<input type=\"submit\" value=\"Search\"></form>%s</body></html>" % (esc(title), STYLE, body))


def result_list(entries):
    out = []
    for e in entries:
        vid = e["id"]
        meta = " &middot; ".join(x for x in [
            esc(e.get("channel")), esc(fmt_views(e.get("views"))),
            "LIVE" if e.get("live") else esc(fmt_dur(e.get("duration")))] if x)
        out.append(
            "<a href=\"/watch?v=%s\"><img src=\"https://i.ytimg.com/vi/%s/mqdefault.jpg\" "
            "width=\"320\" height=\"180\" alt=\"%s\"></a>"
            "<p><a href=\"/watch?v=%s\"><b>%s</b></a><br><span class=\"meta\">%s</span></p>"
            % (vid, vid, esc(e["title"]), vid, esc(e["title"]), meta))
    return "".join(out)


def home_page():
    try:
        entries = search(HOME_QUERY, 12)
    except Exception as e:
        entries = []
        log("home search failed:", e)
    topics = " &middot; ".join("<a href=\"/results?search_query=%s\">%s</a>"
                               % (urllib.parse.quote_plus(t), esc(t)) for t in HOME_TOPICS)
    body = ("<h1>YouTube</h1><p>Browse: %s</p><h2>Popular</h2>%s" % (topics, result_list(entries)))
    return page("YouTube", body)


def results_page(q):
    try:
        entries = search(q)
    except Exception as e:
        return page("YouTube", "<h1>Search failed</h1><p>%s</p>" % esc(e))
    body = "<h2>Results for %s</h2>%s" % (esc(q), result_list(entries) or "<p>No videos found.</p>")
    return page("%s - YouTube" % q, body)


def watch_page(vid):
    # The page does not wait for yt-dlp when a search already said what the
    # video is: it renders from that, and the full extraction runs in the
    # background for the stream request the page is about to make. Only a
    # video nobody has searched for (a pasted link) waits, and not forever.
    info = cached_info(vid)
    if info is None:
        entry = search_entry(vid)
        prefetch_info(vid)
        if entry is None:
            with _lock:
                ev = _inflight.get(vid)
            if ev:
                ev.wait(10)
            info = cached_info(vid)
        if info is None:
            info = {
                "title": (entry or {}).get("title") or "YouTube video",
                "channel": (entry or {}).get("channel") or "",
                "view_count": (entry or {}).get("views"),
                "duration": (entry or {}).get("duration"),
                "description": "",
                "is_live": (entry or {}).get("live"),
            }

    title = info.get("title") or vid
    date = info.get("upload_date") or ""
    if len(date) == 8:
        date = "%s-%s-%s" % (date[:4], date[4:6], date[6:])
    meta = " &middot; ".join(x for x in [
        esc(fmt_views(info.get("view_count"))), esc(date), esc(fmt_dur(info.get("duration")))] if x)

    desc = (info.get("description") or "").strip()
    if len(desc) > 1800:
        desc = desc[:1800].rsplit(" ", 1)[0] + " ..."
    paras = "".join("<p>%s</p>" % esc(p) for p in desc.split("\n") if p.strip())

    more = ""
    with _lock:
        pools = [v[1] for v in _search_cache.values()]
    seen, picks = {vid}, []
    for pool in pools:
        for e in pool:
            if e["id"] not in seen and len(picks) < 6:
                seen.add(e["id"])
                picks.append(e)
    if picks:
        more = "<h3>More videos</h3>" + result_list(picks)

    live = info.get("is_live")
    body = ("<h2>%s</h2>"
            "<video src=\"/stream/%s.mpg\" width=\"640\" height=\"360\"></video>"
            "<p><b>%s</b> &middot; <span class=\"meta\">%s</span></p>"
            "%s<h3>Description</h3>%s%s"
            % (esc(title), vid, esc(info.get("channel") or info.get("uploader")), meta,
               "<p><b>This is a live stream.</b></p>" if live else "",
               paras or "<p class=\"meta\">No description.</p>", more))
    return page("%s - YouTube" % title, body)


# ----------------------------------------------------------------- handler

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "ytgate/1.0"

    def log_message(self, fmt, *a):
        log("%s %s" % (self.client_address[0], fmt % a))

    def send_html(self, text, status=200):
        data = text.encode("utf-8", "replace")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def redirect(self, where):
        self.send_response(302)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path
        try:
            if path in ("/", "/index.html", "/feed/trending"):
                return self.send_html(home_page())
            if path == "/results":
                term = (q.get("search_query") or q.get("q") or [""])[0].strip()
                return self.send_html(results_page(term) if term else home_page())
            if path == "/watch":
                vid = (q.get("v") or [""])[0]
                if len(vid) != 11:
                    return self.send_html(page("YouTube", "<h1>No such video</h1>"), 404)
                return self.send_html(watch_page(vid))
            if path.startswith("/shorts/") or path.startswith("/embed/") or path.startswith("/live/"):
                return self.redirect("/watch?v=" + path.split("/")[2][:11])
            if path.startswith("/stream/") and path.endswith(".mpg"):
                try:
                    quality = int((q.get("q") or [DEFAULT_Q])[0])
                    start = max(0.0, float((q.get("t") or ["0"])[0]))
                except ValueError:
                    quality, start = DEFAULT_Q, 0.0
                if quality not in QUALITIES:
                    quality = min(QUALITIES, key=lambda k: abs(k - quality))
                return self.stream(path[len("/stream/"):-4], quality, start)
            if path.startswith("/ytimg/"):
                return self.thumb(path[len("/ytimg"):])
            if path.startswith("/@") or path.startswith("/c/") or path.startswith("/channel/"):
                name = path.strip("/").split("/")[-1].lstrip("@")
                return self.redirect("/results?search_query=" + urllib.parse.quote_plus(name))
            self.send_html(page("YouTube", "<h1>Not available</h1><p>This part of YouTube is "
                                "not available through the hawkOS gateway. "
                                "<a href=\"/\">Home</a></p>"), 404)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def thumb(self, path):
        global _thumb_bytes
        with _lock:
            data = _thumb_cache.get(path)
        if data is None:
            try:
                req = urllib.request.Request("https://i.ytimg.com" + path,
                                             headers={"User-Agent": "Mozilla/5.0"})
                data = urllib.request.urlopen(req, timeout=20).read()
            except Exception as e:
                log("thumb %s: %s" % (path, e))
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            with _lock:
                if _thumb_bytes + len(data) > THUMB_BUDGET:
                    _thumb_cache.clear()
                    _thumb_bytes = 0
                _thumb_cache[path] = data
                _thumb_bytes += len(data)
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg" if path.endswith(".jpg") else "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "max-age=86400")
        self.end_headers()
        self.wfile.write(data)

    def stream(self, vid, quality=DEFAULT_Q, start=0.0):
        if len(vid) != 11:
            return self.send_html("bad id", 404)
        try:
            info = video_info(vid)
        except Exception as e:
            return self.send_html("video unavailable: %s" % esc(e), 502)
        if info.get("is_live"):
            return self.send_html("live streams are not supported", 501)

        # Download a little above the output size when that is what exists:
        # scaling down hides the source's compression; scaling up shows it.
        vf, af = pick_formats(info, max(quality, 240))
        if not vf:
            return self.send_html("no downloadable format", 502)

        # A refused URL gets one fresh extraction; if YouTube still refuses,
        # yt-dlp downloads the streams itself.
        via_ytdlp = False
        if not reachable(vf) or (af and not reachable(af)):
            try:
                info = video_info(vid, fresh=True)
                vf, af = pick_formats(info, max(quality, 240))
            except Exception as e:
                log("re-extract %s failed: %s" % (vid, e))
            if not vf:
                return self.send_html("no downloadable format", 502)
            if not reachable(vf) or (af and not reachable(af)):
                via_ytdlp = True
                log("stream %s: direct download refused, handing it to yt-dlp" % vid)
        duration = float(info.get("duration") or 0)
        if duration and start > duration - 1:
            start = max(0.0, duration - 1)

        # Where to start each input, and what to trim once there.
        vstart = astart = 0
        vprefix = aprefix = b""
        vskip = askip = 0.0
        begin = start
        vidx = dash_index(vf) if start > 0 and not via_ytdlp else None
        if vidx:
            begin, vstart = seek_point(vidx, start)
            vprefix = vidx[0]
            aidx = dash_index(af) if af else None
            if aidx:
                at, astart = seek_point(aidx, begin)
                aprefix = aidx[0]
                askip = max(0.0, begin - at)
            elif af:
                askip = begin
        elif start > 0:
            vskip = askip = start
        log("stream %s: video %s (%s %sp), audio %s -> %dp@%d from %.1fs%s" % (
            vid, vf.get("format_id"), vf.get("vcodec"), vf.get("height"),
            af.get("format_id") if af else "-", quality, ARGS.fps, begin,
            " (indexed)" if vidx else (" (decoding up to it)" if start > 0 else "")))

        tmp = tempfile.mkdtemp(prefix="ytgate-")
        stop = threading.Event()
        proc = None
        sent = 0
        t0 = time.time()
        try:
            vpath = os.path.join(tmp, "v")
            os.mkfifo(vpath)
            apath = None
            if af:
                apath = os.path.join(tmp, "a")
                os.mkfifo(apath)
            if via_ytdlp:
                threads = [threading.Thread(target=pump_ytdlp, args=(vid, vf, vpath, stop), daemon=True)]
                if af:
                    threads.append(threading.Thread(target=pump_ytdlp, args=(vid, af, apath, stop), daemon=True))
            else:
                def refresh(fid, vid=vid):
                    try:
                        for f in video_info(vid, fresh=True).get("formats") or []:
                            if f.get("format_id") == fid and f.get("url"):
                                return f
                    except Exception as e:
                        log("refresh %s/%s failed: %s" % (vid, fid, e))
                    return None
                threads = [threading.Thread(target=pump, args=(vf, vpath, stop, vstart, vprefix, refresh), daemon=True)]
                if af:
                    threads.append(threading.Thread(target=pump, args=(af, apath, stop, astart, aprefix, refresh), daemon=True))
            for t in threads:
                t.start()

            proc = subprocess.Popen(ffmpeg_cmd(vpath, apath, quality, ARGS.fps, vskip, askip),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.send_response(200)
            self.send_header("Content-Type", "video/mpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Start", "%.3f" % begin)
            self.send_header("X-Duration", "%.3f" % duration)
            self.send_header("X-Quality", str(quality))
            self.end_headers()
            while True:
                b = proc.stdout.read1(32 << 10) if hasattr(proc.stdout, "read1") else proc.stdout.read(32 << 10)
                if not b:
                    break
                self.wfile.write(b)
                sent += len(b)
        except (BrokenPipeError, ConnectionResetError):
            log("stream %s: client went away after %d KB" % (vid, sent >> 10))
        finally:
            stop.set()
            if proc:
                try:
                    proc.kill()
                except Exception:
                    pass
                err = proc.stderr.read().decode(errors="replace").strip() if proc.stderr else ""
                proc.wait()
                if err and sent == 0:
                    log("ffmpeg: " + err[-400:])
            # Unblock any pump still waiting to open its FIFO for writing.
            for name in ("v", "a"):
                fp = os.path.join(tmp, name)
                if os.path.exists(fp):
                    try:
                        fd = os.open(fp, os.O_RDONLY | os.O_NONBLOCK)
                        os.close(fd)
                    except OSError:
                        pass
            shutil.rmtree(tmp, ignore_errors=True)
            log("stream %s: %d KB in %.1fs" % (vid, sent >> 10, time.time() - t0))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global ARGS
    ap = argparse.ArgumentParser(description="YouTube gateway for hawkOS")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--fps", type=int, default=24, help="MPEG-1 allows 24, 25 or 30")
    ARGS = ap.parse_args()

    for tool in ("yt-dlp", "ffmpeg"):
        if not shutil.which(tool):
            sys.exit("ytgate: %s not found on PATH" % tool)

    # Warm the home page so the first visit does not wait on a search.
    threading.Thread(target=lambda: search(HOME_QUERY, 12), daemon=True).start()

    srv = Server((ARGS.host, ARGS.port), Handler)
    log("ytgate listening on http://%s:%d (guest: http://10.0.2.2:%d), %d fps, default %dp"
        % (ARGS.host, ARGS.port, ARGS.port, ARGS.fps, DEFAULT_Q))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
