# Mock Splunk HEC receiver (stdlib only).  docker build -f deploy/docker/mock-hec.Dockerfile -t hotpath-mock-hec:dev .
FROM python:3.12-slim
RUN useradd --system --uid 10001 --no-create-home hec
WORKDIR /app
COPY python/hotpath/__init__.py python/hotpath/mock_hec.py hotpath/
USER 10001
ENV HOTPATH_MOCK_HEC_HOST=0.0.0.0 HOTPATH_MOCK_HEC_PORT=8088
EXPOSE 8088
ENTRYPOINT ["python", "-m", "hotpath.mock_hec"]
