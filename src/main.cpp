#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

#include "hotpath/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
  std::puts(
      "hotpath - streaming anomaly inference engine\n\n"
      "All configuration comes from HOTPATH_* environment variables (see .env.example and the README).\n"
      "Key variables: HOTPATH_MODEL, HOTPATH_SOURCE=synthetic|tcp, HOTPATH_RATE, HOTPATH_TOTAL_EVENTS,\n"
      "HOTPATH_BATCH_SIZE, HOTPATH_WORKERS, HOTPATH_HEC_URL, HOTPATH_HEC_TOKEN, HOTPATH_METRICS_PORT.\n"
      "On exit a one-line JSON summary is printed to stdout.");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    usage();
    return std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0 ? 0 : 2;
  }
  try {
    const hotpath::Config cfg = hotpath::Config::from_env();
    const hotpath::ModelMeta meta = hotpath::ModelMeta::load(cfg.meta_path);
    hotpath::Scorer scorer(cfg.model_path, meta);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    hotpath::Pipeline pipeline(cfg, scorer.as_function(), meta.threshold);
    pipeline.start();
    std::fprintf(stderr,
                 "hotpath: started (source=%s workers=%zu batch=%zu rate=%g threshold=%.5f hec=%s metrics=:%d)\n",
                 cfg.source.c_str(), cfg.workers, cfg.batch_size, cfg.rate, static_cast<double>(meta.threshold),
                 cfg.hec_enabled() ? cfg.hec_url.c_str() : "off", pipeline.metrics_port());

    while (!pipeline.finished() && !g_stop.load()) {
      // A finite synthetic run ends by itself; otherwise wait for SIGINT/SIGTERM.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (g_stop.load()) break;
    }
    pipeline.request_stop();
    pipeline.join();
    if (cfg.print_summary) std::cout << pipeline.summary().to_json() << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hotpath: fatal: %s\n", e.what());
    return 1;
  }
}
