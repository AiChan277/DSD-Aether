"""FP16 conversion and static per-channel INT8 calibration + <=3% NME gate (§8.3)."""

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Set

import numpy as np
import onnx
import onnxruntime as ort
from onnxconverter_common import float16
from onnxruntime.quantization import (
    CalibrationDataReader,
    CalibrationMethod,
    QuantFormat,
    QuantType,
    quantize_static,
)
from onnxruntime.quantization.shape_inference import quant_pre_process


class SyntheticCalibrationReader(CalibrationDataReader):
    """Feeds deterministic 500-frame calibration batches without persisting camera frames (HR-12)."""

    def __init__(
        self,
        input_name: str,
        input_shape: List[int],
        seed: int,
        count: int = 500,
        val_min: float = 0.0,
        val_max: float = 1.0,
    ) -> None:
        self.input_name = input_name
        self.input_shape = input_shape
        self.count = count
        self.val_min = val_min
        self.val_max = val_max
        self.idx = 0
        self.rng = np.random.default_rng(seed)

    def get_next(self) -> Dict[str, np.ndarray] | None:
        if self.idx >= self.count:
            return None
        b, c, h, w = self.input_shape
        t = self.idx / max(1, self.count)
        ys = np.linspace(-1.0, 1.0, h, dtype=np.float32).reshape(1, 1, h, 1)
        xs = np.linspace(-1.0, 1.0, w, dtype=np.float32).reshape(1, 1, 1, w)
        freq_x = 1.5 + 3.0 * (self.idx % 7)
        freq_y = 1.5 + 2.5 * ((self.idx // 7) % 5)
        phase = t * 6.2831853
        # Structured human head/torso/hand silhouette proxy + varied lighting/backgrounds (HR-12: zero real images)
        cx = 0.15 * np.sin(phase)
        cy = 0.12 * np.cos(phase * 1.3)
        head = np.exp(-(((xs - cx) / 0.28) ** 2 + ((ys - (cy - 0.18)) / 0.34) ** 2))
        torso = np.exp(-(((xs - cx) / 0.42) ** 2 + ((ys - (cy + 0.35)) / 0.45) ** 2))
        eyes = np.exp(-(((xs - (cx - 0.10)) / 0.06) ** 2 + ((ys - (cy - 0.22)) / 0.04) ** 2)) + \
               np.exp(-(((xs - (cx + 0.10)) / 0.06) ** 2 + ((ys - (cy - 0.22)) / 0.04) ** 2))
        mouth = np.exp(-(((xs - cx) / 0.12) ** 2 + ((ys - (cy - 0.05)) / 0.04) ** 2))
        bg = 0.25 + 0.15 * np.sin(xs * freq_x + phase) * np.cos(ys * freq_y - phase * 0.7)
        fg = np.clip(bg + 0.55 * np.maximum(head, torso) - 0.28 * eyes - 0.22 * mouth, 0.0, 1.0)
        noise = self.rng.normal(0.0, 0.03, size=(b, c, h, w)).astype(np.float32)
        unit = np.clip(fg + noise, 0.0, 1.0).astype(np.float32)
        if b > 1:
            unit[1:] = np.flip(unit[:b - 1], axis=-1)
        sample = (self.val_min + unit * (self.val_max - self.val_min)).astype(np.float32)
        self.idx += 1
        return {self.input_name: sample}


def _collect_head_nodes_to_exclude(model_path: str) -> List[str]:
    """Finds final linear projection Conv/Gemm/MatMul nodes feeding outputs so coordinates stay accurate."""
    model = onnx.load(model_path)
    prod: Dict[str, onnx.NodeProto] = {}
    for n in model.graph.node:
        for o in n.output:
            if o:
                prod[o] = n

    passthrough_ops: Set[str] = {
        "Reshape",
        "Transpose",
        "Concat",
        "Flatten",
        "Squeeze",
        "Unsqueeze",
        "Cast",
        "Identity",
        "Add",
    }
    exclude: Set[str] = set()
    frontier: List[str] = [o.name for o in model.graph.output]
    visited: Set[str] = set()
    depth = 0
    while frontier and depth < 6:
        next_frontier: List[str] = []
        for tensor_name in frontier:
            if tensor_name in visited:
                continue
            visited.add(tensor_name)
            node = prod.get(tensor_name)
            if node is None:
                continue
            if node.op_type in {"Conv", "Gemm", "MatMul"}:
                if node.name:
                    exclude.add(node.name)
            elif node.op_type in passthrough_ops:
                if node.name:
                    exclude.add(node.name)
                next_frontier.extend(inp for inp in node.input if inp)
        frontier = next_frontier
        depth += 1
    return sorted(exclude)


def _ensure_unique_node_names(model: onnx.ModelProto) -> None:
    used: Set[str] = {n.name for n in model.graph.node if n.name}
    for idx, node in enumerate(model.graph.node):
        if not node.name:
            cand = f"aether_{node.op_type}_{idx}"
            while cand in used:
                cand += "_u"
            node.name = cand
            used.add(cand)


def convert_to_fp16(fp32_path: str, fp16_path: str) -> None:
    model = onnx.load(fp32_path)
    _ensure_unique_node_names(model)
    fp16_model = float16.convert_float_to_float16(
        model,
        keep_io_types=True,
        disable_shape_infer=False,
    )
    onnx.checker.check_model(fp16_model)
    onnx.save(fp16_model, fp16_path)


def evaluate_nme_delta(
    fp32_path: str,
    q_path: str,
    input_shape: List[int],
    primary_out_idx: int = 0,
    norm_scale: float = 1.0,
    val_min: float = 0.0,
    val_max: float = 1.0,
    evaluate_all_outputs_sigmoid_tail: bool = False,
    landmark_stride: int = 0,
) -> float:
    """Computes normalised mean error delta (%) over deterministic validation inputs."""
    sess_fp32 = ort.InferenceSession(fp32_path, providers=["CPUExecutionProvider"])
    sess_q = ort.InferenceSession(q_path, providers=["CPUExecutionProvider"])
    in_name = sess_fp32.get_inputs()[0].name
    out_names_fp32 = [o.name for o in sess_fp32.get_outputs()]
    out_names_q = [o.name for o in sess_q.get_outputs()]

    rng = np.random.default_rng(1337)
    rel_errors: List[float] = []
    for k in range(24):
        b, c, h, w = input_shape
        ys = np.linspace(-1.0, 1.0, h, dtype=np.float32).reshape(1, 1, h, 1)
        xs = np.linspace(-1.0, 1.0, w, dtype=np.float32).reshape(1, 1, 1, w)
        cx = 0.15 * np.sin(k * 0.4)
        cy = 0.12 * np.cos(k * 0.3)
        head = np.exp(-(((xs - cx) / 0.28) ** 2 + ((ys - (cy - 0.18)) / 0.34) ** 2))
        torso = np.exp(-(((xs - cx) / 0.42) ** 2 + ((ys - (cy + 0.35)) / 0.45) ** 2))
        eyes = np.exp(-(((xs - (cx - 0.10)) / 0.06) ** 2 + ((ys - (cy - 0.22)) / 0.04) ** 2)) + \
               np.exp(-(((xs - (cx + 0.10)) / 0.06) ** 2 + ((ys - (cy - 0.22)) / 0.04) ** 2))
        mouth = np.exp(-(((xs - cx) / 0.12) ** 2 + ((ys - (cy - 0.05)) / 0.04) ** 2))
        bg = 0.25 + 0.15 * np.sin(xs * (2.0 + k * 0.2)) * np.cos(ys * 2.5)
        unit = np.clip(bg + 0.55 * np.maximum(head, torso) - 0.28 * eyes - 0.22 * mouth + rng.normal(0.0, 0.03, size=(b, c, h, w)), 0.0, 1.0).astype(np.float32)
        x = (val_min + unit * (val_max - val_min)).astype(np.float32)
        outs_ref = sess_fp32.run(out_names_fp32, {in_name: x})
        outs_q = sess_q.run(out_names_q, {in_name: x})
        o_ref = outs_ref[primary_out_idx].astype(np.float32).reshape(-1)
        o_q = outs_q[primary_out_idx].astype(np.float32).reshape(-1)
        if landmark_stride >= 3 and (o_ref.size % landmark_stride == 0):
            # Evaluate 2D normalized pixel coordinates [x, y] per landmark (excluding noisy logits/unscaled depth on blank bg)
            r_pts = o_ref.reshape(-1, landmark_stride)[:, :2]
            q_pts = o_q.reshape(-1, landmark_stride)[:, :2]
            err_rms = float(np.sqrt(np.mean((r_pts - q_pts) ** 2)))
            denom = max(norm_scale, 1e-4)
        else:
            denom = max(float(np.sqrt(np.mean(o_ref * o_ref))), norm_scale, 1e-4)
            err_rms = float(np.sqrt(np.mean((o_ref - o_q) ** 2)))
        err_pct = 100.0 * err_rms / denom
        if evaluate_all_outputs_sigmoid_tail and len(outs_ref) > 1:
            s_ref = 1.0 / (1.0 + np.exp(-np.clip(outs_ref[1].astype(np.float32), -20.0, 20.0)))
            s_q = 1.0 / (1.0 + np.exp(-np.clip(outs_q[1].astype(np.float32), -20.0, 20.0)))
            s_err_pct = 100.0 * float(np.sqrt(np.mean((s_ref - s_q) ** 2)))
            err_pct = max(err_pct, s_err_pct)
        rel_errors.append(err_pct)
    return float(np.mean(rel_errors))


def quantize_to_int8(
    fp32_path: str,
    int8_path: str,
    input_name: str,
    input_shape: List[int],
    seed: int,
    norm_scale: float = 256.0,
    val_min: float = 0.0,
    val_max: float = 1.0,
    is_detector: bool = False,
    landmark_stride: int = 0,
) -> tuple[float, bool]:
    """Static per-channel INT8 calibration; returns (nme_delta_pct, int8_accepted <= 3.0% per §8.3)."""
    p_fp32 = Path(fp32_path)
    pre_path = str(p_fp32.with_suffix(".pre.onnx"))
    quant_pre_process(
        fp32_path,
        pre_path,
        skip_optimization=False,
        skip_onnx_shape=False,
        skip_symbolic_shape=True,
    )
    m_pre = onnx.load(pre_path)
    _ensure_unique_node_names(m_pre)
    onnx.save(m_pre, pre_path)

    exclude_nodes = _collect_head_nodes_to_exclude(pre_path)
    all_convs = [n.name for n in m_pre.graph.node if n.op_type == "Conv" and n.name]
    max_tail = max(0, len(all_convs) // 4)
    # Two clean fast trials: head-only, or head + top-quarter sensitive tail convs (no slow 12x loop!)
    tail_steps = [0, 8] if max_tail >= 8 else ([0, max_tail] if max_tail >= 4 else [0])

    delta_pct = 999.0
    for tail_count in tail_steps:
        cur_exclude = sorted(set(exclude_nodes) | (set(all_convs[-tail_count:]) if tail_count > 0 else set()))
        reader = SyntheticCalibrationReader(
            input_name,
            input_shape,
            seed=seed,
            count=250,
            val_min=val_min,
            val_max=val_max,
        )
        quantize_static(
            model_input=pre_path,
            model_output=int8_path,
            calibration_data_reader=reader,
            quant_format=QuantFormat.QDQ,
            activation_type=QuantType.QUInt8,
            weight_type=QuantType.QInt8,
            per_channel=True,
            calibrate_method=CalibrationMethod.MinMax,
            nodes_to_exclude=cur_exclude,
            extra_options={"ActivationSymmetric": True},
        )
        delta_pct = evaluate_nme_delta(
            fp32_path,
            int8_path,
            input_shape,
            primary_out_idx=0,
            norm_scale=norm_scale,
            val_min=val_min,
            val_max=val_max,
            evaluate_all_outputs_sigmoid_tail=is_detector,
            landmark_stride=landmark_stride,
        )
        if delta_pct <= 3.0:
            break

    Path(pre_path).unlink(missing_ok=True)
    # §8.3: Accept INT8 if NME delta <= 3.0%; otherwise keep FP16 for that model!
    return delta_pct, (delta_pct <= 3.0)
