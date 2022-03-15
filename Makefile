# Convenience wrapper; every target is a thin shell around documented commands.
export PATH := $(CURDIR)/.venv/bin:$(PATH)
export PYTHONPATH := $(CURDIR)/python
BUILD ?= build

.PHONY: chart demo setup train build test test-py test-cpp bench bench-micro run mock-hec tsan images kind-up kind-down helm-lint clean

setup:            ## create .venv with Python deps (and a pip-provided cmake if none is installed)
	python3 -m venv .venv
	.venv/bin/pip install -q -r requirements-dev.txt

train:            ## train the isolation forest and export models/anomaly.onnx (+ .meta)
	python -m hotpath.train --out models

build:            ## configure + compile engine, tests and benchmarks (downloads ONNX Runtime, GoogleTest, Benchmark)
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$$(nproc)

test: train build test-py test-cpp   ## everything

test-py:
	pytest -q

test-cpp:
	cd $(BUILD) && ctest --output-on-failure

bench: train build   ## C++ vs Python end-to-end comparison -> bench/results/latest.json
	python bench/compare.py

chart:            ## render docs/benchmark.svg from the last `make bench` run
	python bench/make_chart.py

demo:             ## 45 s kind demo for a terminal recording (needs `make kind-up` first)
	scripts/demo.sh

bench-micro:      ## Google Benchmark micro-benchmarks only
	$(BUILD)/bench/hotpath_bench

mock-hec:         ## terminal 1 of the local demo: mock Splunk HEC on :8088
	python -m hotpath.mock_hec

run:              ## terminal 2 of the local demo: score a synthetic stream, metrics on :9100
	$(BUILD)/hotpath

tsan:             ## run the unit tests under ThreadSanitizer
	cmake -S . -B build-tsan -DHOTPATH_SANITIZE=ON -DHOTPATH_BUILD_BENCH=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build-tsan -j$$(nproc)
	build-tsan/tests/cpp/hotpath_tests --gtest_filter='-PipelineModel.*'

images:           ## build both container images
	docker build -f deploy/docker/Dockerfile -t hotpath:dev .
	docker build -f deploy/docker/mock-hec.Dockerfile -t hotpath-mock-hec:dev .

kind-up:          ## build images, create a kind cluster and helm-install the chart
	scripts/kind-up.sh

kind-down:
	scripts/kind-down.sh

helm-lint:
	helm lint deploy/helm/hotpath

clean:
	rm -rf build build-tsan models bench/results .pytest_cache
