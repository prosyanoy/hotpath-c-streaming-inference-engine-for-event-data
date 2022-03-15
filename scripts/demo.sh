#!/usr/bin/env bash
# ~45 s demo for a terminal recording (e.g. `asciinema rec -c scripts/demo.sh demo.cast`):
#   1. helm install onto the kind cluster  2. ramp the load generator  3. live metrics view
# Prerequisite: `make kind-up` once (builds images, creates the cluster). Re-running is safe.
set -euo pipefail
cd "$(dirname "$0")/.."
CLUSTER="${KIND_CLUSTER:-hotpath}"; NS="${HELM_NAMESPACE:-hotpath}"; REL="${HELM_RELEASE:-hotpath}"
CTX="kind-$CLUSTER"

echo "\$ helm upgrade --install $REL deploy/helm/hotpath   # baseline: 5k events/s"
helm upgrade --install "$REL" deploy/helm/hotpath --kube-context "$CTX" -n "$NS" --create-namespace --wait --timeout 120s | head -3
kubectl --context "$CTX" -n "$NS" get pods

kubectl --context "$CTX" -n "$NS" port-forward "svc/$REL" 9100:9100 >/dev/null 2>&1 &
PF=$!; trap 'kill $PF 2>/dev/null || true' EXIT
for _ in $(seq 20); do curl -fs localhost:9100/readyz >/dev/null && break; sleep 0.5; done

echo; echo "\$ python scripts/live_metrics.py   # 10 s at the baseline rate"
python3 scripts/live_metrics.py http://localhost:9100/metrics 10

echo; echo "\$ helm upgrade ... --set config.rate=0   # load generator: unthrottled synthetic ingest"
helm upgrade "$REL" deploy/helm/hotpath --kube-context "$CTX" -n "$NS" --reuse-values --set config.rate=0 --wait --timeout 120s >/dev/null
kill $PF 2>/dev/null || true
kubectl --context "$CTX" -n "$NS" port-forward "svc/$REL" 9100:9100 >/dev/null 2>&1 &
PF=$!
for _ in $(seq 20); do curl -fs localhost:9100/readyz >/dev/null && break; sleep 0.5; done
python3 scripts/live_metrics.py http://localhost:9100/metrics 20

echo; echo "\$ curl localhost:8088/stats   # alerts that reached the mock Splunk HEC"
kubectl --context "$CTX" -n "$NS" exec "deploy/$REL-mock-hec" -- python -c \
  "import urllib.request,json;d=json.load(urllib.request.urlopen('http://localhost:8088/stats'));print({k:d[k] for k in ('events_received','requests','rejected')})"
