#!/usr/bin/env bash
# Build both images, create (or reuse) a local kind cluster and install the Helm chart.
set -euo pipefail
cd "$(dirname "$0")/.."

CLUSTER="${KIND_CLUSTER:-hotpath}"
RELEASE="${HELM_RELEASE:-hotpath}"
NAMESPACE="${HELM_NAMESPACE:-hotpath}"

for tool in docker kind kubectl helm; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

docker build -f deploy/docker/Dockerfile -t hotpath:dev .
docker build -f deploy/docker/mock-hec.Dockerfile -t hotpath-mock-hec:dev .

if ! kind get clusters | grep -qx "$CLUSTER"; then
  kind create cluster --name "$CLUSTER"
fi
kind load docker-image hotpath:dev hotpath-mock-hec:dev --name "$CLUSTER"

helm upgrade --install "$RELEASE" deploy/helm/hotpath \
  --kube-context "kind-$CLUSTER" --namespace "$NAMESPACE" --create-namespace --wait --timeout 120s "$@"

cat <<MSG

Deployed. Try:
  kubectl --context kind-$CLUSTER -n $NAMESPACE port-forward svc/$RELEASE 9100:9100 &
  curl -s localhost:9100/metrics | grep -E 'hotpath_(events_scored|anomalies|alerts_sent)_total'
  kubectl --context kind-$CLUSTER -n $NAMESPACE port-forward svc/$RELEASE-mock-hec 8088:8088 &
  curl -s localhost:8088/stats
MSG
