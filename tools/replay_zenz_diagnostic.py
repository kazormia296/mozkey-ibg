#!/usr/bin/env python3
"""Replay opt-in Zenz diagnostic JSONL captures.

The scorer records the exact UTF-8 JSON body sent to llama-server as
``http_json_base64``.  This tool replays those bodies in capture order, so a
single request and the complete preceding request sequence can be compared
without rebuilding the prompt through a different Unicode or JSON layer.

Examples:

  # Replay every captured scorer request against an already running server.
  python tools/replay_zenz_diagnostic.py capture.jsonl \
      --url http://127.0.0.1:18080/completion

  # Replay one generation against a fresh llama-server process.
  python tools/replay_zenz_diagnostic.py capture.jsonl \
      --generation 123 --llama-server /path/to/llama-server \
      --model /path/to/model.gguf --api-key local-test-key

The capture contains user text and model output by design.  Keep it local and
delete it when the investigation is complete.
"""

from __future__ import annotations

import argparse
import base64
import collections
import contextlib
import dataclasses
import http.client
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Iterator, Sequence
from urllib.parse import urlsplit


STOP_MARKERS = tuple(
    [chr(0xEE00 + offset) for offset in range(16)]
    + ["<s>", "</s>", "<unk>", "<|endoftext|>", "\r", "\n"]
)
MAX_HTTP_RESPONSE_BYTES = 64 * 1024


class ReplayError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class CapturedRequest:
    line_number: int
    record: dict[str, Any]
    body: bytes


def _decode_base64(record: dict[str, Any], field: str) -> bytes:
    value = record.get(field)
    if not isinstance(value, str):
        raise ReplayError(f"capture record is missing {field}")
    try:
        return base64.b64decode(value.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError) as exc:
        raise ReplayError(f"invalid base64 in {field}") from exc


def _load_records(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    try:
        with path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ReplayError(
                        f"invalid JSON on capture line {line_number}"
                    ) from exc
                if not isinstance(record, dict):
                    raise ReplayError(
                        f"capture line {line_number} is not a JSON object"
                    )
                record["_line_number"] = line_number
                records.append(record)
    except OSError as exc:
        raise ReplayError(f"cannot read capture: {path}") from exc
    return records


def _select_requests(
    records: Sequence[dict[str, Any]],
    *,
    generation: int | None,
    request_kind: set[str],
    exclude_kind: set[str],
    skip_sequences: set[int],
    skip_lines: set[int],
    limit: int | None,
) -> list[CapturedRequest]:
    selected: list[CapturedRequest] = []
    for record in records:
        if record.get("event") != "scorer_request":
            continue
        kind = record.get("request_kind", "")
        if not isinstance(kind, str):
            continue
        if request_kind and kind not in request_kind:
            continue
        if kind in exclude_kind:
            continue
        if generation is not None and record.get("generation") != generation:
            continue
        sequence = record.get("sequence")
        if isinstance(sequence, int) and sequence in skip_sequences:
            continue
        line_number = record.get("_line_number")
        if isinstance(line_number, int) and line_number in skip_lines:
            continue
        body = _decode_base64(record, "http_json_base64")
        selected.append(CapturedRequest(line_number, record, body))
        if limit is not None and len(selected) >= limit:
            break
    if not selected:
        raise ReplayError("capture contains no matching scorer_request records")
    return selected


def _clean_generated_text(text: str, max_output_chars: int | None = None) -> str:
    end = len(text)
    for marker in STOP_MARKERS:
        position = text.find(marker)
        if position != -1:
            end = min(end, position)
    text = text[:end].strip(" \t\r\n")
    if max_output_chars and max_output_chars > 0:
        text = text[:max_output_chars]
    return text


def _body_for_replay(body: bytes, cache_prompt: str) -> tuple[bytes, list[str]]:
    if cache_prompt == "preserve":
        return body, []
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReplayError("cannot modify a non-JSON captured request") from exc
    if not isinstance(payload, dict):
        raise ReplayError("captured HTTP body is not a JSON object")
    payload["cache_prompt"] = cache_prompt == "true"
    # This mode deliberately changes the request.  Exact-byte replay is the
    # default; modified requests are still UTF-8 and deterministic.
    return (
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        ),
        ["cache_prompt"],
    )


def _parse_content(response_body: bytes) -> tuple[str, str | None]:
    try:
        payload = json.loads(response_body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return "", "response_body_not_json"
    if not isinstance(payload, dict):
        return "", "response_body_not_object"
    content = payload.get("content")
    if not isinstance(content, str):
        return "", "content_field_not_found"
    return content, None


def _post(
    url: str,
    body: bytes,
    *,
    api_key: str,
    timeout: float,
    startup_deadline: float | None,
) -> tuple[int, bytes, str]:
    parsed = urlsplit(url)
    if parsed.scheme != "http" or not parsed.hostname:
        raise ReplayError("--url must be an http:// URL")
    port = parsed.port or 80
    target = parsed.path or "/completion"
    if parsed.query:
        target += "?" + parsed.query

    last_error = ""
    while True:
        connection: http.client.HTTPConnection | None = None
        try:
            connection = http.client.HTTPConnection(
                parsed.hostname, port, timeout=max(0.1, timeout)
            )
            headers = {
                "Content-Type": "application/json; charset=utf-8",
                "Connection": "close",
            }
            if api_key:
                headers["Authorization"] = f"Bearer {api_key}"
            connection.request("POST", target, body=body, headers=headers)
            response = connection.getresponse()
            response_body = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
            if len(response_body) > MAX_HTTP_RESPONSE_BYTES:
                raise ReplayError("HTTP response is larger than 64 KiB")
            return response.status, response_body, ""
        except (ConnectionError, OSError, TimeoutError) as exc:
            last_error = type(exc).__name__
            if startup_deadline is None or time.monotonic() >= startup_deadline:
                return 0, b"", last_error
            time.sleep(0.1)
        finally:
            if connection is not None:
                connection.close()


def _expected_responses(
    records: Sequence[dict[str, Any]],
) -> dict[tuple[int, str], collections.deque[dict[str, Any]]]:
    expected: dict[tuple[int, str], collections.deque[dict[str, Any]]] = {}
    for record in records:
        if record.get("event") != "scorer_response":
            continue
        generation = record.get("generation")
        kind = record.get("request_kind")
        if not isinstance(generation, int) or not isinstance(kind, str):
            continue
        expected.setdefault((generation, kind), collections.deque()).append(record)
    return expected


def _runtime_metadata(request: CapturedRequest) -> dict[str, Any]:
    runtime_args = request.record.get("runtime_args")
    return runtime_args if isinstance(runtime_args, dict) else {}


@contextlib.contextmanager
def _started_server(
    args: argparse.Namespace,
    requests: Sequence[CapturedRequest],
) -> Iterator[subprocess.Popen[bytes] | None]:
    if not args.llama_server:
        yield None
        return

    first = requests[0].record
    runtime_args = _runtime_metadata(requests[0])
    model = args.model or first.get("model_path")
    if not isinstance(model, str) or not model:
        raise ReplayError("--model is required when starting llama-server")
    parsed = urlsplit(args.url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 18080
    ctx = args.ctx or runtime_args.get("ctx") or 256
    threads = args.threads or runtime_args.get("threads") or 4
    device = args.device
    if device is None:
        captured_device = runtime_args.get("device", "")
        device = "" if captured_device in ("", "auto") else str(captured_device)

    command = [
        str(args.llama_server),
        "-m",
        model,
        "-c",
        str(ctx),
        "-t",
        str(threads),
        "--host",
        host,
        "--port",
        str(port),
    ]
    if args.api_key:
        command.extend(["--api-key", args.api_key])
    if device:
        command.extend(["--device", device])

    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError as exc:
        raise ReplayError("cannot start llama-server") from exc
    try:
        yield process
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


def replay(args: argparse.Namespace) -> int:
    records = _load_records(args.capture)
    requests = _select_requests(
        records,
        generation=args.generation,
        request_kind=set(args.request_kind),
        exclude_kind=set(args.exclude_kind),
        skip_sequences=set(args.skip_sequence),
        skip_lines=set(args.skip_line),
        limit=args.limit,
    )
    if args.mode == "single":
        index = args.single_index
        if index < 0 or index >= len(requests):
            raise ReplayError("--single-index is outside the selected requests")
        requests = [requests[index]]

    expected = _expected_responses(records)
    output_stream = (
        args.output.open("w", encoding="utf-8") if args.output else sys.stdout
    )
    try:
        with _started_server(args, requests) as process:
            startup_deadline = (
                time.monotonic() + args.startup_timeout
                if process is not None
                else None
            )
            for replay_index, request in enumerate(requests):
                body, modified_fields = _body_for_replay(
                    request.body, args.cache_prompt
                )
                status, response_body, error = _post(
                    args.url,
                    body,
                    api_key=args.api_key,
                    timeout=args.timeout,
                    startup_deadline=startup_deadline,
                )
                raw_content, parse_error = _parse_content(response_body)
                max_output_chars = 0
                try:
                    payload = json.loads(body.decode("utf-8"))
                    if isinstance(payload, dict):
                        max_output_chars = int(payload.get("n_predict", 0))
                except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
                    pass
                runtime_args = _runtime_metadata(request)
                captured_output_limit = runtime_args.get("max_output_chars")
                if (
                    isinstance(captured_output_limit, int)
                    and captured_output_limit > 0
                ):
                    max_output_chars = captured_output_limit
                clean_content = _clean_generated_text(
                    raw_content, max_output_chars or None
                )

                generation = request.record.get("generation", 0)
                kind = request.record.get("request_kind", "")
                expected_queue = expected.get((generation, kind))
                expected_record = expected_queue.popleft() if expected_queue else None
                expected_clean = None
                if expected_record is not None:
                    field = expected_record.get("clean_generated_text_base64")
                    if isinstance(field, str):
                        try:
                            expected_clean = base64.b64decode(
                                field.encode("ascii"), validate=True
                            ).decode("utf-8")
                        except (ValueError, UnicodeDecodeError):
                            expected_clean = None

                replay_record: dict[str, Any] = {
                    "event": "replay_response",
                    "replay_index": replay_index,
                    "capture_line": request.line_number,
                    "capture_sequence": request.record.get("sequence"),
                    "generation": generation,
                    "request_kind": kind,
                    "modified_fields": modified_fields,
                    "http_status": status,
                    "error": error or parse_error,
                    "raw_http_response_body_base64": base64.b64encode(
                        response_body
                    ).decode("ascii"),
                    "raw_model_output_base64": base64.b64encode(
                        raw_content.encode("utf-8")
                    ).decode("ascii"),
                    "clean_generated_text_base64": base64.b64encode(
                        clean_content.encode("utf-8")
                    ).decode("ascii"),
                    "clean_generated_text": clean_content,
                }
                if expected_clean is not None:
                    replay_record["matches_captured_clean_text"] = (
                        expected_clean == clean_content
                    )
                output_stream.write(json.dumps(replay_record, ensure_ascii=False))
                output_stream.write("\n")
                output_stream.flush()
                print(
                    f"replay[{replay_index}] generation={generation} "
                    f"status={status} output={clean_content!r}",
                    file=sys.stderr,
                )
    finally:
        if args.output:
            output_stream.close()
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="diagnostic JSONL capture")
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:18080/completion",
        help="llama-server completion URL",
    )
    parser.add_argument("--api-key", default="", help="Bearer token, if enabled")
    parser.add_argument(
        "--mode", choices=("sequence", "single"), default="sequence"
    )
    parser.add_argument("--single-index", type=int, default=0)
    parser.add_argument("--generation", type=int)
    parser.add_argument(
        "--request-kind",
        action="append",
        default=[],
        help="include only this request kind (repeatable)",
    )
    parser.add_argument(
        "--exclude-kind", action="append", default=[], help="exclude request kind"
    )
    parser.add_argument(
        "--skip-sequence", type=int, action="append", default=[],
        help="skip a capture sequence number (repeatable)",
    )
    parser.add_argument(
        "--skip-line", type=int, action="append", default=[],
        help="skip a JSONL line number (repeatable)",
    )
    parser.add_argument("--limit", type=int)
    parser.add_argument(
        "--cache-prompt",
        choices=("preserve", "true", "false"),
        default="preserve",
        help="preserve or override cache_prompt in the replay body",
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--startup-timeout", type=float, default=120.0)
    parser.add_argument("--llama-server", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--ctx", type=int)
    parser.add_argument("--threads", type=int)
    parser.add_argument("--device")
    parser.add_argument("--output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        return replay(args)
    except ReplayError as exc:
        print(f"replay failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
