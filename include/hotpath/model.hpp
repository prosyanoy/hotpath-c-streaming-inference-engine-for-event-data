#pragma once
#include <functional>
#include <memory>
#include <string>

namespace hotpath {

/// Contents of the ``<model>.meta`` file written by python/hotpath/train.py.
struct ModelMeta {
  std::string input_name = "X";
  std::string score_output = "scores";
  std::size_t n_features = 0;
  float threshold = 0;  // score < threshold => anomaly

  static ModelMeta load(const std::string& path);  // throws std::runtime_error
  static ModelMeta parse(const std::string& text);
};

/// Scores a row-major [n x kFeatures] matrix into `scores[n]`. Must be thread-safe.
using ScoreFn = std::function<void(const float* features, std::size_t n, float* scores)>;

/// ONNX Runtime CPU session. One session is shared by all workers (Session::Run is thread-safe);
/// inputs are wrapped in place and outputs are written into caller-owned memory, so scoring does not
/// copy features or allocate result buffers.
class Scorer {
 public:
  Scorer(const std::string& model_path, const ModelMeta& meta);
  ~Scorer();
  void score(const float* features, std::size_t n, float* scores) const;
  ScoreFn as_function() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hotpath
