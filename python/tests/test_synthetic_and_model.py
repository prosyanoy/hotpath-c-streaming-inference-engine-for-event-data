import numpy as np
import onnx
import onnxruntime as ort

from hotpath import synthetic, train


def test_generate_shapes_and_bounds():
    x, y = synthetic.generate(5000, 0.1, np.random.default_rng(0))
    assert x.shape == (5000, synthetic.N_FEATURES) and x.dtype == np.float32
    assert 0.05 < y.mean() < 0.15
    assert (x >= 0).all() and (x[:, :2] <= 100).all()


def test_anomalies_are_far_from_normal():
    x, y = synthetic.generate(20000, 0.05, np.random.default_rng(1))
    z = np.abs((x - synthetic.MEAN) / synthetic.STD).max(axis=1)
    assert np.median(z[y == 1]) > 4.5 > np.median(z[y == 0])


def test_train_export_and_detect(tmp_path):
    model, threshold = train.train(seed=3, n_train=15000, n_estimators=50)
    onnx_path = train.export(model, threshold, tmp_path)
    meta = dict(l.split("=", 1) for l in (tmp_path / "anomaly.meta").read_text().split())
    assert meta["n_features"] == "8" and float(meta["threshold"]) == float(f"{threshold:.9g}")

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    x, y = synthetic.generate(10000, 0.05, np.random.default_rng(9))
    scores = sess.run([meta["score_output"]], {meta["input_name"]: x})[0].ravel()
    # The fused ONNX forest must reproduce scikit-learn (float32 thresholds can flip a rare borderline row).
    close = np.isclose(scores, model.decision_function(x), atol=1e-4)
    assert close.mean() > 0.999
    assert len(onnx.load(str(onnx_path)).graph.node) == 4  # one fused forest, not ~3000 nodes
    flagged = scores < float(meta["threshold"])
    assert flagged[y == 1].mean() > 0.4      # recall on injected anomalies
    assert flagged[y == 0].mean() < 0.02     # false-positive rate near calibration target
