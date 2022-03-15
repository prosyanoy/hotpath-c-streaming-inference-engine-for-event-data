"""Train an isolation-forest anomaly model on synthetic telemetry and export it to ONNX.

Outputs (default ``models/``):
  * ``anomaly.onnx``  - float32 input ``X`` [N, 8]; output ``scores`` [N, 1] (= sklearn
                        ``decision_function``; lower means more anomalous)
  * ``anomaly.meta``  - key=value file read by the C++ service and the Python baseline
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper
from sklearn.ensemble import IsolationForest

from . import synthetic


def train(seed: int = 7, n_train: int = 60_000, n_estimators: int = 100, target_fpr: float = 0.005):
    rng = np.random.default_rng(seed)
    x_train, _ = synthetic.generate(n_train, anomaly_rate=0.01, rng=rng)
    model = IsolationForest(n_estimators=n_estimators, max_samples=256, random_state=seed)
    model.fit(x_train)
    # Calibrate the alert threshold on clean validation data: flag the lowest `target_fpr`.
    x_val, _ = synthetic.generate(40_000, anomaly_rate=0.0, rng=rng)
    threshold = float(np.quantile(model.decision_function(x_val), target_fpr))
    return model, threshold


EULER_GAMMA = 0.5772156649015329


def _avg_path_length(n: float) -> float:
    """Expected path length of an unsuccessful BST search (sklearn's ``_average_path_length``)."""
    if n <= 1:
        return 0.0
    if n == 2:
        return 1.0
    return 2.0 * (np.log(n - 1.0) + EULER_GAMMA) - 2.0 * (n - 1.0) / n


def build_onnx(model: IsolationForest) -> onnx.ModelProto:
    """Fuse the whole forest into ONE ``TreeEnsembleRegressor`` plus a few element-wise ops.

    skl2onnx emits one sub-graph per tree (~3000 nodes for 100 trees), so per-node dispatch cost
    dominates small batches. Here each leaf stores ``(depth + c(n_leaf)) / (n_trees * c(max_samples))``
    and the ensemble SUMs them, giving the normalised mean path length ``d``; then
    ``decision_function = 0.5 - 2**(-d)`` exactly as scikit-learn computes it for ``contamination='auto'``.
    """
    n_trees = len(model.estimators_)
    norm = n_trees * _avg_path_length(model.max_samples_)
    a = {k: [] for k in ("treeids", "nodeids", "featureids", "values", "modes", "true", "false")}
    t_tree, t_node, t_weight = [], [], []
    for t, (est, feats) in enumerate(zip(model.estimators_, model.estimators_features_)):
        tree = est.tree_
        left, right = tree.children_left, tree.children_right
        depth = np.zeros(tree.node_count)
        for node in range(tree.node_count):  # sklearn numbers children after their parent
            if left[node] != -1:
                depth[left[node]] = depth[right[node]] = depth[node] + 1
        for node in range(tree.node_count):
            a["treeids"].append(t)
            a["nodeids"].append(node)
            if left[node] == -1:
                a["featureids"].append(0); a["values"].append(0.0); a["modes"].append("LEAF")
                a["true"].append(0); a["false"].append(0)
                t_tree.append(t); t_node.append(node)
                t_weight.append(float(depth[node] + _avg_path_length(tree.n_node_samples[node])) / norm)
            else:
                a["featureids"].append(int(feats[tree.feature[node]]))
                a["values"].append(float(tree.threshold[node])); a["modes"].append("BRANCH_LEQ")
                a["true"].append(int(left[node])); a["false"].append(int(right[node]))
    forest = helper.make_node(
        "TreeEnsembleRegressor", ["X"], ["depth"], domain="ai.onnx.ml", n_targets=1, aggregate_function="SUM",
        post_transform="NONE", nodes_treeids=a["treeids"], nodes_nodeids=a["nodeids"],
        nodes_featureids=a["featureids"], nodes_values=a["values"], nodes_modes=a["modes"],
        nodes_truenodeids=a["true"], nodes_falsenodeids=a["false"],
        target_treeids=t_tree, target_nodeids=t_node, target_ids=[0] * len(t_tree), target_weights=t_weight)
    nodes = [
        forest,
        helper.make_node("Neg", ["depth"], ["neg_depth"]),
        helper.make_node("Pow", ["two", "neg_depth"], ["p"]),
        helper.make_node("Sub", ["half", "p"], ["scores"]),
    ]
    consts = [helper.make_tensor("two", TensorProto.FLOAT, [1], [2.0]),
              helper.make_tensor("half", TensorProto.FLOAT, [1], [0.5])]
    graph = helper.make_graph(
        nodes, "hotpath_isolation_forest",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [None, synthetic.N_FEATURES])],
        [helper.make_tensor_value_info("scores", TensorProto.FLOAT, [None, 1])], initializer=consts)
    m = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17), helper.make_opsetid("ai.onnx.ml", 3)])
    m.ir_version = 9
    onnx.checker.check_model(m)
    return m


def export(model: IsolationForest, threshold: float, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = out_dir / "anomaly.onnx"
    onnx_path.write_bytes(build_onnx(model).SerializeToString())
    meta = {
        "input_name": "X",
        "score_output": "scores",
        "n_features": synthetic.N_FEATURES,
        "threshold": f"{threshold:.9g}",
        "features": ",".join(synthetic.FEATURES),
    }
    (out_dir / "anomaly.meta").write_text("".join(f"{k}={v}\n" for k, v in meta.items()))
    return onnx_path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="models", type=Path)
    ap.add_argument("--seed", default=7, type=int)
    ap.add_argument("--n-train", default=60_000, type=int)
    ap.add_argument("--n-estimators", default=100, type=int)
    args = ap.parse_args(argv)
    model, threshold = train(args.seed, args.n_train, args.n_estimators)
    path = export(model, threshold, args.out)
    print(f"wrote {path} (threshold={threshold:.6f})")


if __name__ == "__main__":
    main()
