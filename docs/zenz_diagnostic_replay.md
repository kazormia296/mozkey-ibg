# Zenz diagnostic capture and replay

Zenz prompt and model-output capture is deliberately opt-in because the JSONL
file contains the user's surrounding text and model responses. Set
`MOZC_ZENZ_DIAGNOSTIC_JSONL` to an **absolute local file path** before starting
Mozkey. The same inherited environment reaches `mozc_zenz_scorer`, so Session
and scorer events are appended to one JSONL stream even when the scorer uses a
different working directory.

On Windows, the diagnostic build gives the persistent per-user
`MOZC_ZENZ_DIAGNOSTIC_JSONL` value priority over the process environment. This
covers servers launched by a TSF or broker process that retained an older
inherited value; capture remains disabled when neither value is present.

The capture includes:

- `session_request`: raw/context fields, exact prompt bytes as
  `prompt_base64`, prompt code points, protected-span count, and request
  options.
- `scorer_request`: exact `/completion` JSON as `http_json_base64`, runtime
  arguments, and the prompt bytes.
- `scorer_response`: raw HTTP response body, raw model `content`, and the
  text after `CleanGeneratedText`.
- `session_stage` and `session_decision`: the response after context stripping,
  placeholder/symbol restoration, Validator, AdoptionPolicy, and display.

The scorer computes `runtime_sha256` and `model_sha256` from the files used for
the request. `MOZC_ZENZ_RUNTIME_SHA256` and `MOZC_ZENZ_MODEL_SHA256` may be set
to precomputed values when the files are unavailable to the scorer; these
environment variables do not enable capture.

Replay the exact scorer request sequence against a running local server:

```text
python tools/replay_zenz_diagnostic.py capture.jsonl \
  --url http://127.0.0.1:18080/completion
```

Replay only the request that produced generation 123 against a fresh server:

```text
python tools/replay_zenz_diagnostic.py capture.jsonl \
  --generation 123 \
  --llama-server /path/to/llama-server \
  --model /path/to/zenz-v3.2-small-Q5_K_M.gguf \
  --api-key local-test-key
```

The default is byte-preserving replay. For request-history delta debugging,
use `--skip-line` or `--skip-sequence`; to test the cache hypothesis use
`--cache-prompt false`. The output JSONL reports the raw response, cleaned
text, and whether the cleaned text matched the captured scorer response. Run a
new server process for each independent fresh-runtime trial.

The known malformed strings such as `璋†堯賄賂`, `漉з諷滉紘紘耀`, and
`瀛槽￥跋扈矜矜` are actual model/output or post-processing observations, not
encoding-decoding examples; they can be compared directly through the
recorded Base64 fields.
