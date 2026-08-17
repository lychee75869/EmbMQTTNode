#!/usr/bin/env python3
"""
EmbMQTTNode Isolation Forest Model Trainer & C Exporter

Stages:
  1. Generate synthetic 3D sensor data (temperature/humidity/pressure)
     with injected anomalies
  2. Train Isolation Forest (scikit-learn)
  3. Export model as C header file (src/iforest_model.h)

Anomaly injection types:
  - Spike: sudden jump in temperature/humidity/pressure (sensor fault)
  - Drift: linear drift over time (environmental runaway)
  - Outlier: single extreme point (data acquisition error)

Usage:
  pip install numpy scikit-learn
  python tools/anomaly_train.py

Output:
  src/iforest_model.h  (overwrites stub, ready for #include)
"""

import numpy as np
from sklearn.ensemble import IsolationForest
import os
import sys

# ─── 配置 ────────────────────────────────────────────────
N_TREES       = 100        # 树的数量
MAX_SAMPLES   = 256        # 每棵树的子采样数
CONTAMINATION = 0.05       # 训练数据中预期的异常比例
N_NORMAL      = 5000       # 正常样本数
N_ANOMALY     = 250        # 异常样本数
RANDOM_SEED   = 42

OUTPUT_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "src", "iforest_model.h"
)


def generate_synthetic_data():
    """
    Generate synthetic 3D sensor data with injected anomalies.

    Normal baseline:
      temperature: N(25.0, 2.0)  °C
      humidity:    N(55.0, 8.0)  %RH
      pressure:    N(1013.0, 4.0) hPa

    Anomaly injection (5% total):
      1. Spike: temperature ±15°C (sensor fault)
      2. Drift: humidity ±25% (environmental runaway)
      3. Outlier: pressure ±30 hPa (acquisition error)
    """
    rng = np.random.RandomState(RANDOM_SEED)

    # ── 正常数据 ──
    normal = np.column_stack([
        rng.normal(25.0, 2.0, N_NORMAL),    # temperature
        rng.normal(55.0, 8.0, N_NORMAL),    # humidity
        rng.normal(1013.0, 4.0, N_NORMAL),  # pressure
    ])

    # ── 异常数据 ──
    n_each = N_ANOMALY // 3
    anomalies = []

    # 类型 1: 温度突变
    temp_spike = np.column_stack([
        rng.normal(40.0, 2.0, n_each),      # 高温 40°C
        rng.normal(55.0, 8.0, n_each),
        rng.normal(1013.0, 4.0, n_each),
    ])
    anomalies.append(temp_spike)

    # 类型 2: 湿度漂移
    humid_drift = np.column_stack([
        rng.normal(25.0, 2.0, n_each),
        rng.normal(20.0, 5.0, n_each),      # 极低湿度 20%
        rng.normal(1013.0, 4.0, n_each),
    ])
    anomalies.append(humid_drift)

    # 类型 3: 气压离群
    press_outlier = np.column_stack([
        rng.normal(25.0, 2.0, n_each),
        rng.normal(55.0, 8.0, n_each),
        rng.normal(985.0, 3.0, n_each),     # 极低气压 985hPa
    ])
    anomalies.append(press_outlier)

    # 附加：多维异常（温度+湿度同时偏离）
    extra = N_ANOMALY - 3 * n_each
    if extra > 0:
        multi = np.column_stack([
            rng.normal(42.0, 2.0, extra),   # 高温
            rng.normal(15.0, 3.0, extra),   # 极干
            rng.normal(1013.0, 4.0, extra),
        ])
        anomalies.append(multi)

    anomaly_data = np.vstack(anomalies)

    # 合并
    X = np.vstack([normal, anomaly_data])
    y = np.hstack([
        np.zeros(N_NORMAL, dtype=int),
        np.ones(N_ANOMALY, dtype=int),
    ])

    # 打乱
    idx = rng.permutation(len(X))
    X, y = X[idx], y[idx]

    return X, y


def train_iforest(X):
    """Train Isolation Forest model on synthetic sensor data."""
    print(f"[1/4] Training Isolation Forest...")
    print(f"       n_estimators={N_TREES}, max_samples={MAX_SAMPLES}, "
          f"contamination={CONTAMINATION}")

    model = IsolationForest(
        n_estimators=N_TREES,
        max_samples=MAX_SAMPLES,
        contamination=CONTAMINATION,
        random_state=RANDOM_SEED,
        n_jobs=-1,
    )
    model.fit(X)

    # 统计
    scores = model.decision_function(X)
    n_anomaly_pred = np.sum(model.predict(X) == -1)
    print(f"       samples={len(X)}, predicted_anomalies={n_anomaly_pred}")
    print(f"       score range: [{scores.min():.4f}, {scores.max():.4f}]")
    return model


def extract_trees(model):
    """
    Extract tree structures from a trained IsolationForest model.

    Returns per tree:
      nodes: list of [feature, split_value, left_child, right_child]
      node_count: total nodes (internal + leaf)

    Internal node: feature and split_value, children point to sub-nodes
    Leaf node:     feature=-1, split=0, left=-1, right=-1
    """
    print(f"[2/4] Extracting tree structures...")

    all_trees = []
    max_nodes = 0

    for i, tree in enumerate(model.estimators_):
        # scikit-learn 内部树结构
        tree_obj = tree.tree_
        n_nodes  = tree_obj.node_count

        nodes = []
        for j in range(n_nodes):
            feature = tree_obj.feature[j]
            split   = tree_obj.threshold[j]
            left    = tree_obj.children_left[j]
            right   = tree_obj.children_right[j]

            # scikit-learn 用 -2 表示叶节点
            if feature == -2:
                feature = -1
                split   = 0.0
                left    = -1
                right   = -1

            nodes.append([feature, split, left, right])

        all_trees.append(nodes)
        if n_nodes > max_nodes:
            max_nodes = n_nodes

    print(f"       trees={len(all_trees)}, max_nodes_per_tree={max_nodes}")
    return all_trees


def export_c_header(trees, X_sample, scores_sample):
    """Export tree structures as a C header file (src/iforest_model.h)."""
    print(f"[3/4] Exporting C header to {OUTPUT_PATH}...")

    n_trees   = len(trees)
    max_nodes = max(len(t) for t in trees)

    # 构建扁平数组
    flat_trees = []
    offsets    = []
    offset     = 0

    for tree in trees:
        offsets.append(offset)
        n_nodes = len(tree)
        flat_trees.append(n_nodes)
        offset += 1  # node_count
        for node in tree:
            flat_trees.extend(node)  # feature, split, left, right
            offset += 4

    # 生成测试向量：取 10 个样本用于 C 侧交叉验证
    n_test = min(10, len(X_sample))
    test_indices = np.linspace(0, len(X_sample)-1, n_test, dtype=int)
    test_vectors = X_sample[test_indices]
    test_scores  = scores_sample[test_indices]

    # ── 写文件 ──
    with open(OUTPUT_PATH, "w") as f:
        f.write("/*\n")
        f.write(" * iforest_model.h\n")
        f.write(" * AUTO-GENERATED by tools/anomaly_train.py\n")
        f.write(f" * Trees: {n_trees}, Max nodes/tree: {max_nodes}\n")
        f.write(" * DO NOT EDIT BY HAND\n")
        f.write(" */\n")
        f.write("#ifndef IFOREST_MODEL_H\n")
        f.write("#define IFOREST_MODEL_H\n\n")

        f.write(f"#define IFOREST_AVAILABLE 1\n")
        f.write(f"#define IFOREST_N_TREES   {n_trees}\n")
        f.write(f"#define IFOREST_MAX_NODES {max_nodes}\n\n")

        # 扁平树数组
        f.write("/* Flattened tree structures:\n")
        f.write(" *   Each tree: [node_count, feat0, split0, left0, right0, ...]\n")
        f.write(" *   feat=-1 → leaf node\n")
        f.write(" */\n")
        f.write(f"static const float iforest_trees[] = {{\n")
        line = ""
        for i, val in enumerate(flat_trees):
            if isinstance(val, float):
                line += f"{val:.6f}f, "
            else:
                line += f"{int(val)}, "
            if (i + 1) % 8 == 0:
                f.write(f"    {line}\n")
                line = ""
        if line:
            f.write(f"    {line}\n")
        f.write("};\n\n")

        # 偏移数组
        f.write("/* Tree offsets into iforest_trees[] */\n")
        f.write(f"static const int iforest_tree_offsets[] = {{\n")
        for i, off in enumerate(offsets):
            f.write(f"    {off}")
            if i < len(offsets) - 1:
                f.write(",")
            f.write("\n")
        f.write("};\n\n")

        # 测试向量
        f.write("/* Test vectors for cross-validation */\n")
        f.write(f"#define IFOREST_TEST_VECTORS {n_test}\n")
        f.write("static const double iforest_test_inputs[] = {\n")
        for vec in test_vectors:
            f.write(f"    {vec[0]:.3f}, {vec[1]:.3f}, {vec[2]:.3f},\n")
        f.write("};\n\n")
        f.write("static const double iforest_test_expected[] = {\n")
        for score in test_scores:
            f.write(f"    {score:.8f},\n")
        f.write("};\n\n")

        f.write("#endif /* IFOREST_MODEL_H */\n")

    print(f"       wrote {len(flat_trees)} values ({n_trees} trees)")
    print(f"       + {n_test} cross-validation test vectors")
    return n_test


def verify_model(model, X, y):
    """Evaluate model quality (precision, recall, score range)."""
    print(f"[4/4] Verifying model quality...")

    from sklearn.metrics import classification_report

    y_pred = model.predict(X)
    y_pred_binary = (y_pred == -1).astype(int)

    # 调整 scores 到 0-1 范围（0=正常, 1=异常）
    scores = model.decision_function(X)
    scores_norm = 1.0 - (scores - scores.min()) / (scores.max() - scores.min())

    tp = np.sum((y_pred_binary == 1) & (y == 1))
    fp = np.sum((y_pred_binary == 1) & (y == 0))
    fn = np.sum((y_pred_binary == 0) & (y == 1))
    tn = np.sum((y_pred_binary == 0) & (y == 0))

    precision = tp / (tp + fp) if (tp + fp) > 0 else 0
    recall    = tp / (tp + fn) if (tp + fn) > 0 else 0

    print(f"       precision={precision:.3f}, recall={recall:.3f}")
    print(f"       TP={tp}, FP={fp}, FN={fn}, TN={tn}")
    print(f"       score_norm range: [{scores_norm.min():.4f}, {scores_norm.max():.4f}]")

    # 建议阈值
    print(f"\n  Suggested C anomaly threshold: 0.55 ~ 0.65")
    print(f"  (lower = more sensitive, higher = fewer false alarms)")

    return scores_norm


def main():
    print("=" * 60)
    print(" EmbMQTTNode Isolation Forest Model Trainer")
    print("=" * 60)
    print()

    X, y = generate_synthetic_data()
    print(f"    Data: {len(X)} samples, {np.sum(y)} anomalies ({100*np.sum(y)/len(X):.1f}%)")
    print()

    model = train_iforest(X)
    trees = extract_trees(model)
    scores = verify_model(model, X, y)

    export_c_header(trees, X, scores)

    print(f"\n=== Done: model exported to {OUTPUT_PATH} ===")
    print(f"  Include in C code: #include \"iforest_model.h\"")
    print(f"  Trees: {len(trees)}, estimated size: ~{len(trees)*64/1024:.0f}KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
