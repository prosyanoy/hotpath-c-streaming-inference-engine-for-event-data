#include "hotpath/model.hpp"

#include <onnxruntime_cxx_api.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "hotpath/event.hpp"

namespace hotpath {

ModelMeta ModelMeta::parse(const std::string& text) {
  ModelMeta m;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "input_name") m.input_name = v;
    else if (k == "score_output") m.score_output = v;
    else if (k == "n_features") m.n_features = static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
    else if (k == "threshold") {
      char* end = nullptr;
      m.threshold = std::strtof(v.c_str(), &end);
      if (end == v.c_str()) throw std::runtime_error("model meta: bad threshold '" + v + "'");
    }
  }
  if (m.n_features != kFeatures)
    throw std::runtime_error("model meta: n_features=" + std::to_string(m.n_features) + ", engine is built for " +
                             std::to_string(kFeatures));
  return m;
}

ModelMeta ModelMeta::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open model meta file '" + path + "' (run `make train` first?)");
  std::stringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

struct Scorer::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "hotpath"};
  Ort::Session session{nullptr};
  Ort::MemoryInfo mem{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
  std::string input_name, output_name;
};

Scorer::Scorer(const std::string& model_path, const ModelMeta& meta) : impl_(std::make_unique<Impl>()) {
  if (!std::ifstream(model_path)) throw std::runtime_error("cannot open model '" + model_path + "' (run `make train` first?)");
  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(1);  // parallelism comes from our own worker threads
  opts.SetInterOpNumThreads(1);
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  impl_->session = Ort::Session(impl_->env, model_path.c_str(), opts);
  impl_->input_name = meta.input_name;
  impl_->output_name = meta.score_output;
}

Scorer::~Scorer() = default;

void Scorer::score(const float* features, std::size_t n, float* scores) const {
  if (n == 0) return;
  const int64_t in_shape[2] = {static_cast<int64_t>(n), static_cast<int64_t>(kFeatures)};
  const int64_t out_shape[2] = {static_cast<int64_t>(n), 1};
  Ort::Value input = Ort::Value::CreateTensor<float>(impl_->mem, const_cast<float*>(features), n * kFeatures, in_shape, 2);
  Ort::Value output = Ort::Value::CreateTensor<float>(impl_->mem, scores, n, out_shape, 2);
  const char* in_names[] = {impl_->input_name.c_str()};
  const char* out_names[] = {impl_->output_name.c_str()};
  impl_->session.Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, &output, 1);
}

ScoreFn Scorer::as_function() const {
  return [this](const float* f, std::size_t n, float* s) { score(f, n, s); };
}

}  // namespace hotpath
