# Minimal Ray Serve app: N HTTP requests -> 1 batched GPU kernel.
# Run: serve run serve_batch:app
import os

import nvtx
import torch
from ray import serve
from starlette.requests import Request

DEVICE = os.environ.get("DEVICE", "cuda")


@serve.deployment(
    max_ongoing_requests=16,  # default (5) would cap batches at 5
    ray_actor_options={"num_gpus": 1 if DEVICE == "cuda" else 0},
)
class Model:
    def __init__(self):
        self.w = torch.randn(1024, 1024, device=DEVICE)

    @serve.batch(max_batch_size=8, batch_wait_timeout_s=0.5)
    async def infer(self, traceparents: list[str], xs: list[float]) -> list[float]:
        x = torch.tensor(xs, device=DEVICE).unsqueeze(1).expand(-1, 1024)
        # One kernel serves every request in the batch: publish all their
        # trace contexts so a profiler can attribute the GPU work to them.
        with nvtx.annotate("otel.batch " + " ".join(traceparents), domain="otel"):
            y = (x @ self.w).sum(dim=1)
        print(f"batch of {len(xs)}: {traceparents}", flush=True)
        return y.tolist()

    async def __call__(self, req: Request):
        tp = req.headers.get("traceparent", "")
        x = float(req.query_params.get("x", 1))
        return {"y": await self.infer(tp, x)}


app = Model.bind()
