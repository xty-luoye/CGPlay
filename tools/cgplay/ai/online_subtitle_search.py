#!/usr/bin/env python3
import argparse
import base64
import gzip
import json
import os
import sys
import urllib.parse
import urllib.request
import urllib.error
from difflib import SequenceMatcher


def _result(success, **kwargs):
    payload = {"success": success}
    payload.update(kwargs)
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if success else 1


def _write_report(path, payload):
    if not path:
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)


def _api_key(args):
    return (args.api_key or os.environ.get("SUBTITLE_ONLINE_API_KEY") or "").strip()


def _base_url(args):
    return (args.base_url or os.environ.get("SUBTITLE_ONLINE_BASE_URL") or "https://api.opensubtitles.com/api/v1").rstrip("/")


def _workbench_api_key(args):
    return (
        args.api_key
        or os.environ.get("SUBTITLE_WORKBENCH_API_KEY")
        or os.environ.get("SUBTITLE_TRANSLATION_API_KEY")
        or os.environ.get("AI_API_KEY")
        or os.environ.get("OPENAI_API_KEY")
        or ""
    ).strip()


def _workbench_base_url(args):
    return (
        args.base_url
        or os.environ.get("SUBTITLE_WORKBENCH_BASE_URL")
        or os.environ.get("SUBTITLE_TRANSLATION_BASE_URL")
        or os.environ.get("AI_BASE_URL")
        or os.environ.get("OPENAI_BASE_URL")
        or ""
    ).rstrip("/")


def _workbench_model(args):
    return (
        args.model
        or os.environ.get("SUBTITLE_WORKBENCH_MODEL")
        or os.environ.get("SUBTITLE_TRANSLATION_MODEL")
        or os.environ.get("AI_MODEL")
        or os.environ.get("OPENAI_MODEL")
        or "gpt-4o-mini"
    ).strip()


def _request_json(url, api_key):
    req = urllib.request.Request(url)
    req.add_header("Api-Key", api_key)
    req.add_header("User-Agent", "CGPlay/1.0")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def _chat_endpoint(base_url):
    value = (base_url or "").rstrip("/")
    if not value:
        return ""
    if value.endswith("/chat/completions"):
        return value
    if value.endswith("/v1"):
        return value + "/chat/completions"
    return value + "/v1/chat/completions"


def _json_from_text(text):
    value = (text or "").strip()
    if value.startswith("```"):
        value = value.strip("`")
        if value.lower().startswith("json"):
            value = value[4:].strip()
    start = value.find("{")
    end = value.rfind("}")
    if start >= 0 and end > start:
        value = value[start:end + 1]
    return json.loads(value)


def _chat_content(data):
    choices = data.get("choices") or []
    if choices:
        message = choices[0].get("message") or {}
        content = message.get("content", "")
        if isinstance(content, list):
            return "\n".join(part.get("text", "") for part in content if isinstance(part, dict))
        return str(content or "")
    return str(data.get("output_text") or data.get("text") or "")


def _workbench_search(args, metadata, common):
    api_key = _workbench_api_key(args)
    base_url = _workbench_base_url(args)
    model = _workbench_model(args)
    endpoint = _chat_endpoint(base_url)
    common.update({
        "onlineProvider": "workbench-api",
        "provider": "workbench-api",
        "endpoint": endpoint or base_url,
        "model": model,
        "workbenchConfigSource": "smart-recognition",
        "workbenchCanSearchSubtitles": True,
        "onlineReferenceProviderKind": "workbench-online-reference-provider",
    })
    if not api_key or not endpoint:
        return {
            **common,
            "success": False,
            "error": "workbench-api-key-or-base-url-missing",
            "rejectReason": "workbench-api-key-or-base-url-missing",
            "fallbackReason": "workbench-api-key-or-base-url-missing",
            "onlineSearchResultCount": 0,
            "onlineSearchAcceptedCount": 0,
        }
    system = (
        "You are CGPlay's high-quality subtitle workbench. You may use your online/search capability "
        "to find subtitle or reference subtitle text for the provided media metadata. Do not ask for or "
        "require video/audio upload. Return JSON only."
    )
    user = {
        "task": "search_subtitles_or_reference_subtitles",
        "constraints": [
            "use only the metadata below",
            "do not request video/audio upload",
            "prefer official or matching Traditional/Simplified Chinese, Japanese, or English subtitles",
            "if you can retrieve subtitle text, return it as WebVTT in vttText",
            "if not, return candidates and a concrete failure reason"
        ],
        "metadata": metadata,
        "responseSchema": {
            "success": "boolean",
            "candidates": [{"title": "string", "language": "string", "sourceUrl": "string", "confidence": "number", "license": "string"}],
            "selectedCandidate": "object|null",
            "vttText": "string optional",
            "fallbackReason": "string"
        }
    }
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": json.dumps(user, ensure_ascii=False)}
        ],
        "temperature": 0.1,
    }
    req = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            status = getattr(resp, "status", 200)
            body = resp.read().decode("utf-8", errors="replace")
        data = json.loads(body)
        parsed = _json_from_text(_chat_content(data))
        candidates = parsed.get("candidates") or []
        selected = parsed.get("selectedCandidate") or (candidates[0] if candidates else None)
        vtt_text = str(parsed.get("vttText") or parsed.get("subtitleVtt") or "").strip()
        cache_written = False
        coverage = {"cueCount": 0, "coverageEndSeconds": 0.0, "coverage": 0.0}
        if vtt_text:
            os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
            with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(vtt_text if vtt_text.startswith("WEBVTT") else "WEBVTT\n\n" + vtt_text)
            coverage = _coverage_from_text(vtt_text)
            cache_written = coverage["cueCount"] > 0
        success = bool(cache_written)
        return {
            **common,
            "success": success,
            "httpStatus": status,
            "onlineSourceKind": "workbench-online-reference",
            "onlineSearchResultCount": len(candidates),
            "onlineSearchAcceptedCount": 1 if success else 0,
            "candidateCount": len(candidates),
            "candidates": candidates,
            "selectedCandidate": selected,
            "onlineLanguage": (selected or {}).get("language", "") if isinstance(selected, dict) else "",
            "onlineCoverage": coverage["coverage"],
            "onlineCoverageEndSeconds": coverage["coverageEndSeconds"],
            "onlineCueCount": coverage["cueCount"],
            "alignmentScore": float((selected or {}).get("confidence", 0.0)) if isinstance(selected, dict) else 0.0,
            "offsetMs": 0,
            "fallbackReason": "" if success else (parsed.get("fallbackReason") or "workbench-search-returned-no-vtt"),
            "workbenchRawResponseParsed": True,
        }
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace") if exc.fp else ""
        return {
            **common,
            "success": False,
            "httpStatus": exc.code,
            "error": body[:1200] or str(exc),
            "rejectReason": "workbench-online-search-http-error",
            "fallbackReason": "workbench-online-search-http-error",
            "onlineSearchResultCount": 0,
            "onlineSearchAcceptedCount": 0,
        }
    except Exception as exc:
        return {
            **common,
            "success": False,
            "error": str(exc),
            "rejectReason": "workbench-online-search-failed",
            "fallbackReason": "workbench-online-search-failed",
            "onlineSearchResultCount": 0,
            "onlineSearchAcceptedCount": 0,
        }


def _download(url, api_key):
    req = urllib.request.Request(url)
    req.add_header("Api-Key", api_key)
    req.add_header("User-Agent", "CGPlay/1.0")
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("utf-8", errors="replace")


def _query_from_media(path):
    base = os.path.splitext(os.path.basename(path))[0]
    for token in ("1080p", "720p", "2160p", "x264", "x265", "h264", "h265", "web-dl", "bdrip"):
        base = base.replace(token, " ")
        base = base.replace(token.upper(), " ")
    return " ".join(base.replace(".", " ").replace("_", " ").split())


def _file_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def _metadata(args, query):
    return {
        "filename": os.path.basename(args.media),
        "title": query,
        "query": query,
        "duration": args.duration,
        "language": args.language,
        "moviehash": (args.moviehash or "").strip(),
        "fileSize": args.file_size or _file_size(args.media),
        "season": args.season,
        "episode": args.episode,
    }


def _candidate_from_item(item, query, provider, raw_index=0):
    attributes = item.get("attributes") or {}
    feature = attributes.get("feature_details") or {}
    files = attributes.get("files") or []
    language = attributes.get("language") or attributes.get("lang") or ""
    release = attributes.get("release") or attributes.get("movie_name") or feature.get("title") or ""
    votes = attributes.get("votes") or 0
    downloads = attributes.get("download_count") or attributes.get("downloads") or 0
    trusted = bool(attributes.get("from_trusted") or attributes.get("trusted"))
    exactish = bool(feature) or bool(attributes.get("moviehash_match"))
    title_score = SequenceMatcher(None, query.lower(), str(release).lower()).ratio() if release else 0.0
    confidence = 0.35 + min(0.35, title_score * 0.35)
    if exactish:
        confidence += 0.20
    if trusted:
        confidence += 0.05
    if votes:
        confidence += min(0.05, float(votes) / 200.0)
    if downloads:
        confidence += min(0.05, float(downloads) / 10000.0)
    confidence = max(0.0, min(0.98, confidence))
    source_url = attributes.get("url") or attributes.get("related_links") or ""
    if isinstance(source_url, list):
        source_url = source_url[0].get("url", "") if source_url and isinstance(source_url[0], dict) else ""
    return {
        "provider": provider,
        "rawIndex": raw_index,
        "id": str(item.get("id") or attributes.get("subtitle_id") or ""),
        "language": language,
        "release": release,
        "confidence": round(confidence, 3),
        "sourceUrl": source_url,
        "license": attributes.get("license") or attributes.get("copyright") or "provider-terms",
        "status": "downloadable" if files else "no-downloadable-file",
        "fileId": files[0].get("file_id") if files else None,
    }


def _coverage_from_text(text):
    # Lightweight parser for diagnostics only; the player performs authoritative parsing.
    import re
    last_end = 0.0
    count = 0
    pattern = re.compile(r"(?P<h>\d{1,2}):(?P<m>\d{2}):(?P<s>\d{2})[,.](?P<ms>\d{3})\s+-->\s+(?P<h2>\d{1,2}):(?P<m2>\d{2}):(?P<s2>\d{2})[,.](?P<ms2>\d{3})")
    for match in pattern.finditer(text):
        end = (int(match.group("h2")) * 3600 + int(match.group("m2")) * 60 +
               int(match.group("s2")) + int(match.group("ms2")) / 1000.0)
        last_end = max(last_end, end)
        count += 1
    return {"cueCount": count, "coverageEndSeconds": round(last_end, 3), "coverage": round(last_end, 3)}


def main():
    parser = argparse.ArgumentParser(description="Search/download subtitles through an OpenSubtitles-compatible REST API.")
    parser.add_argument("--media", required=True)
    parser.add_argument("--output", required=True, help="Output subtitle cache path, usually *.online.source.vtt or *.srt")
    parser.add_argument("--search-report", default="", help="Output search diagnostics, usually *.online.search.json")
    parser.add_argument("--language", default="zh,ja,en")
    parser.add_argument("--query", default="")
    parser.add_argument("--duration", type=int, default=0)
    parser.add_argument("--provider", default="opensubtitles-compatible")
    parser.add_argument("--moviehash", default="")
    parser.add_argument("--file-size", type=int, default=0)
    parser.add_argument("--season", type=int, default=0)
    parser.add_argument("--episode", type=int, default=0)
    parser.add_argument("--api-key", default="")
    parser.add_argument("--base-url", default="")
    parser.add_argument("--model", default="")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    provider = args.provider.strip() or "opensubtitles-compatible"
    base_url = _base_url(args)
    query = args.query.strip() or _query_from_media(args.media)
    metadata = _metadata(args, query)
    common = {
        "onlineSearchEnabled": True,
        "onlineProvider": provider,
        "provider": provider,
        "endpoint": base_url,
        "query": metadata,
        "queryText": query,
        "languages": args.language,
        "sentMinimalQueryOnly": True,
        "uploadedMediaPayload": False,
        "candidateCount": 0,
        "candidates": [],
        "selectedCandidate": None,
        "onlineCoverage": 0.0,
        "alignmentScore": 0.0,
        "offsetMs": 0,
    }
    if provider.lower() == "workbench-api":
        payload = _workbench_search(args, metadata, common)
        _write_report(args.search_report, payload)
        return _result(bool(payload.get("success")), **{k: v for k, v in payload.items() if k != "success"})
    api_key = _api_key(args)
    if not api_key:
        payload = {
            "success": False,
            "error": "online-search-api-key-missing",
            "rejectReason": "online-search-api-key-missing",
            "fallbackReason": "online-search-api-key-missing",
            "onlineReferenceProviderKind": "explicit-online-search-provider",
            "onlineSearchResultCount": 0,
            "onlineSearchAcceptedCount": 0,
        }
        payload.update(common)
        _write_report(args.search_report, payload)
        return _result(False, **{k: v for k, v in payload.items() if k != "success"})

    params = {"query": query, "languages": args.language}
    if args.duration > 0:
        params["duration"] = str(args.duration)
    if args.moviehash:
        params["moviehash"] = args.moviehash
    if args.file_size or _file_size(args.media):
        params["moviebytesize"] = str(args.file_size or _file_size(args.media))
    if args.season:
        params["season_number"] = str(args.season)
    if args.episode:
        params["episode_number"] = str(args.episode)
    search_url = f"{base_url}/subtitles?{urllib.parse.urlencode(params)}"
    try:
        search = _request_json(search_url, api_key)
        items = search.get("data") or []
        candidates = [_candidate_from_item(item, query, provider, index) for index, item in enumerate(items)]
        candidates.sort(key=lambda item: item.get("confidence", 0.0), reverse=True)
        if not items:
            payload = {
                "success": False,
                "error": "online-search-no-match",
                "rejectReason": "online-search-no-match",
                "fallbackReason": "online-search-no-match",
                "onlineSourceKind": "opensubtitles-rest",
                "onlineReferenceProviderKind": "explicit-online-search-provider",
                "onlineSearchResultCount": 0,
                "onlineSearchAcceptedCount": 0,
            }
            payload.update(common)
            _write_report(args.search_report, payload)
            return _result(False, **{k: v for k, v in payload.items() if k != "success"})
        selected = next((c for c in candidates if c.get("status") == "downloadable"), candidates[0])
        raw_index = int(selected.get("rawIndex", 0) or 0)
        best = items[raw_index] if 0 <= raw_index < len(items) else items[0]
        attributes = best.get("attributes") or {}
        files = attributes.get("files") or []
        if not files:
            payload = {
                "success": False,
                "error": "online-search-match-has-no-downloadable-file",
                "rejectReason": "online-search-match-has-no-downloadable-file",
                "fallbackReason": "online-search-match-has-no-downloadable-file",
                "onlineSearchResultCount": len(items),
                "onlineSearchAcceptedCount": 0,
                "candidateCount": len(candidates),
                "candidates": candidates,
                "selectedCandidate": selected,
                "match": attributes,
            }
            payload.update({k: v for k, v in common.items() if k not in payload})
            _write_report(args.search_report, payload)
            return _result(False, **{k: v for k, v in payload.items() if k != "success"})
        file_id = selected.get("fileId") or files[0].get("file_id")
        confidence = float(selected.get("confidence", 0.0))
        if args.dry_run:
            payload = {
                "success": True,
                "onlineSourceKind": "opensubtitles-rest",
                "onlineReferenceProviderKind": "explicit-online-search-provider",
                "onlineMatchConfidence": confidence,
                "usedOnlineSubtitle": False,
                "onlineSearchResultCount": len(items),
                "onlineSearchAcceptedCount": 1 if confidence >= 0.60 else 0,
                "candidateCount": len(candidates),
                "candidates": candidates,
                "selectedCandidate": selected,
                "onlineLanguage": selected.get("language", ""),
                "alignmentScore": confidence,
                "offsetMs": 0,
                "fallbackReason": "dry-run-no-download",
                "match": attributes,
            }
            payload.update({k: v for k, v in common.items() if k not in payload})
            _write_report(args.search_report, payload)
            return _result(True, **{k: v for k, v in payload.items() if k != "success"})
        download_url = f"{base_url}/download?{urllib.parse.urlencode({'file_id': file_id})}"
        download_info = _request_json(download_url, api_key)
        link = download_info.get("link") or download_info.get("url")
        if not link:
            payload = {"success": False, "error": "online subtitle download link missing",
                       "rejectReason": "download-link-missing", "fallbackReason": "download-link-missing",
                       "candidateCount": len(candidates), "candidates": candidates,
                       "selectedCandidate": selected}
            payload.update({k: v for k, v in common.items() if k not in payload})
            _write_report(args.search_report, payload)
            return _result(False, **{k: v for k, v in payload.items() if k != "success"})
        text = _download(link, api_key)
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        coverage = _coverage_from_text(text)
        payload = {
            "success": True,
            "onlineSourceKind": "opensubtitles-rest",
            "onlineReferenceProviderKind": "explicit-online-search-provider",
            "onlineMatchConfidence": confidence,
            "usedOnlineSubtitle": True,
            "onlineSearchResultCount": len(items),
            "onlineSearchAcceptedCount": 1,
            "candidateCount": len(candidates),
            "candidates": candidates,
            "selectedCandidate": selected,
            "onlineLanguage": selected.get("language", ""),
            "onlineCoverage": coverage["coverage"],
            "onlineCoverageEndSeconds": coverage["coverageEndSeconds"],
            "onlineCueCount": coverage["cueCount"],
            "alignmentScore": confidence,
            "offsetMs": 0,
            "fallbackReason": "",
            "output": args.output,
            "match": attributes,
        }
        payload.update({k: v for k, v in common.items() if k not in payload})
        _write_report(args.search_report, payload)
        return _result(True, **{k: v for k, v in payload.items() if k != "success"})
    except Exception as exc:
        payload = {
            "success": False,
            "error": str(exc),
            "rejectReason": "online-worker-exception",
            "fallbackReason": "online-worker-exception",
            "onlineSourceKind": "opensubtitles-rest",
            "onlineReferenceProviderKind": "explicit-online-search-provider",
            "onlineSearchResultCount": 0,
            "onlineSearchAcceptedCount": 0,
        }
        payload.update(common)
        _write_report(args.search_report, payload)
        return _result(False, **{k: v for k, v in payload.items() if k != "success"})


if __name__ == "__main__":
    raise SystemExit(main())
