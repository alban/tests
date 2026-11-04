# ray-serve-batch-nvtx

Demo of request batching on a GPU, to discuss trace/span correlation in
GPU profiling ([opentelemetry-ebpf-profiler#2065](https://github.com/open-telemetry/opentelemetry-ebpf-profiler/pull/2065)).

[Ray Serve dynamic batching](https://docs.ray.io/en/latest/serve/advanced-guides/dyn-req-batch.html)
(`@serve.batch`) groups concurrent HTTP requests into one call, so one GPU
kernel does work for several requests, each with its own trace. A profiler
that reads the trace context of the launching thread sees at most one span
(or none: the batch runs in a different asyncio task than the requests).

The app publishes the `traceparent` of every request in the batch as an NVTX
range around the kernel launch, in domain `otel`. This is a hypothetical
convention: no profiler parses it today.

## Run

```sh
pip install "ray[serve]" torch nvtx
serve run serve_batch:app             # with a GPU
DEVICE=cpu serve run serve_batch:app  # without a GPU (NVTX is then a no-op)
./client.sh                           # in another terminal
```

The server log shows batches such as `batch of 8: ['00-…-01', …]`.

## Limitations

- NVTX range names are free text; a `traceparent` is 55 characters, and
  consumers may truncate long names, so this does not scale to large batches.
- NVTX numeric payloads cannot hold a 128-bit trace ID. The structured
  payload extension (`nvToolsExtPayload.h`) is C API.
- An alternative with no new convention: create one span per batch with
  [span links](https://opentelemetry.io/docs/concepts/signals/traces/#span-links)
  to the request spans, and let the profiler attach the batch span through the
  usual thread context.
