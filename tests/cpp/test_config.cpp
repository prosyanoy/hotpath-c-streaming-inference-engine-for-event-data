#include <gtest/gtest.h>

#include <map>
#include <string>

#include "hotpath/config.hpp"
#include "hotpath/model.hpp"

using namespace hotpath;

namespace {
Config load(std::map<std::string, std::string> env) {
  return Config::from_env([&](const char* n) -> const char* {
    auto it = env.find(n);
    return it == env.end() ? nullptr : it->second.c_str();
  });
}
}  // namespace

TEST(Config, Defaults) {
  const Config c = load({});
  EXPECT_EQ(c.source, "synthetic");
  EXPECT_EQ(c.overflow, Overflow::Block);
  EXPECT_EQ(c.meta_path, "models/anomaly.meta");
  EXPECT_TRUE(c.hec_enabled());
}

TEST(Config, ReadsOverrides) {
  const Config c = load({{"HOTPATH_MODEL", "/m/x.onnx"}, {"HOTPATH_WORKERS", "4"}, {"HOTPATH_RATE", "1500.5"},
                         {"HOTPATH_HEC_URL", ""}, {"HOTPATH_SOURCE", "tcp"}});
  EXPECT_EQ(c.meta_path, "/m/x.meta");
  EXPECT_EQ(c.workers, 4u);
  EXPECT_DOUBLE_EQ(c.rate, 1500.5);
  EXPECT_FALSE(c.hec_enabled());
  EXPECT_EQ(c.overflow, Overflow::Drop);  // tcp defaults to dropping
  EXPECT_EQ(load({{"HOTPATH_SOURCE", "tcp"}, {"HOTPATH_OVERFLOW", "block"}}).overflow, Overflow::Block);
}

TEST(Config, RejectsBadValues) {
  EXPECT_THROW(load({{"HOTPATH_WORKERS", "abc"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_WORKERS", "0"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_BATCH_SIZE", "12x"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_SOURCE", "kafka"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_OVERFLOW", "maybe"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_ANOMALY_RATE", "2"}}), std::invalid_argument);
  EXPECT_THROW(load({{"HOTPATH_POOL_BATCHES", "2"}}), std::invalid_argument);  // needs workers + 2
}

TEST(ModelMeta, ParsesAndValidates) {
  const auto m = ModelMeta::parse("input_name=X\nscore_output=scores\nn_features=8\nthreshold=-0.043\nfeatures=a,b\n");
  EXPECT_EQ(m.input_name, "X");
  EXPECT_EQ(m.n_features, 8u);
  EXPECT_FLOAT_EQ(m.threshold, -0.043f);
  EXPECT_THROW(ModelMeta::parse("n_features=5\nthreshold=0\n"), std::runtime_error);
  EXPECT_THROW(ModelMeta::load("/nonexistent/anomaly.meta"), std::runtime_error);
}
