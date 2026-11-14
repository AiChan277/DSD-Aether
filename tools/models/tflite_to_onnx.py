"""Deterministic TFLite -> static ONNX opset-17 converter for AETHER-2.5D (§8.3).

Supports Google MediaPipe Apache-2.0 models (BlazeFace, FaceMesh, FaceBlendshapes,
PoseLandmarker, HandLandmarker). Keeps vision CNN backbones natively in NCHW
(`[N, C, H, W]`) to avoid redundant runtime transposes on Intel NPU and DirectML.
"""

from __future__ import annotations

import math
from typing import Dict, List, Tuple

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper
import tflite


# TFLite TensorType enum -> numpy dtype
_TFLITE_DTYPES = {
    0: np.float32,   # FLOAT32
    1: np.float16,   # FLOAT16
    2: np.int32,     # INT32
    3: np.uint8,     # UINT8
    4: np.int64,     # INT64
    6: np.bool_,     # BOOL
    7: np.int16,     # INT16
    9: np.int8,      # INT8
}

_OP_NAMES = {
    v: k
    for k, v in vars(tflite.BuiltinOperator).items()
    if isinstance(v, int)
}


def _decode_tensor_data(model: tflite.Model, tensor: tflite.Tensor) -> np.ndarray | None:
    buf_idx = tensor.Buffer()
    if buf_idx <= 0:
        return None
    buf = model.Buffers(buf_idx)
    raw = buf.DataAsNumpy()
    if raw is None or (isinstance(raw, int) and raw == 0) or len(raw) == 0:
        return None
    dtype = _TFLITE_DTYPES.get(tensor.Type())
    if dtype is None:
        raise ValueError(f"Unsupported TFLite buffer dtype enum {tensor.Type()}")
    shape = [int(tensor.Shape(k)) for k in range(tensor.ShapeLength())]
    arr = np.frombuffer(bytes(raw), dtype=dtype).copy()
    if shape:
        arr = arr.reshape(shape)
    return arr


def _same_padding_Explicit(in_h: int, in_w: int, k_h: int, k_w: int, s_h: int, s_w: int, d_h: int = 1, d_w: int = 1) -> List[int]:
    eff_kh = (k_h - 1) * d_h + 1
    eff_kw = (k_w - 1) * d_w + 1
    out_h = math.ceil(in_h / s_h)
    out_w = math.ceil(in_w / s_w)
    pad_h = max(0, (out_h - 1) * s_h + eff_kh - in_h)
    pad_w = max(0, (out_w - 1) * s_w + eff_kw - in_w)
    pt = pad_h // 2
    pb = pad_h - pt
    pl = pad_w // 2
    pr = pad_w - pl
    return [pt, pl, pb, pr]


def convert_tflite_to_onnx(
    tflite_bytes: bytes,
    out_onnx_path: str,
    model_name: str,
    batch_override: int | None = None,
    transpose_image_to_nchw: bool = True,
    clean_input_names: List[str] | None = None,
    clean_output_names: List[str] | None = None,
) -> onnx.ModelProto:
    m = tflite.Model.GetRootAsModel(tflite_bytes, 0)
    sg = m.Subgraphs(0)

    num_tensors = sg.TensorsLength()
    consts: Dict[int, np.ndarray] = {}
    t_names: Dict[int, str] = {}
    t_shapes: Dict[int, List[int]] = {}
    t_is_nchw: Dict[int, bool] = {}

    for i in range(num_tensors):
        t = sg.Tensors(i)
        raw_name = t.Name().decode("utf-8", errors="replace") if t.Name() else f"t_{i}"
        t_names[i] = f"t{i}_{raw_name.replace(':', '_').replace('/', '_')}"
        shape = [int(t.Shape(k)) for k in range(t.ShapeLength())]
        if batch_override is not None and len(shape) >= 1 and shape[0] == 1:
            shape[0] = batch_override
        t_shapes[i] = shape
        arr = _decode_tensor_data(m, t)
        if arr is not None:
            consts[i] = arr
        t_is_nchw[i] = False

    # Pre-fold constant DEQUANTIZE operations (FP16 constant -> FP32 constant).
    folded_ops = set()
    for i in range(sg.OperatorsLength()):
        op = sg.Operators(i)
        oc = m.OperatorCodes(op.OpcodeIndex())
        code = max(oc.BuiltinCode(), oc.DeprecatedBuiltinCode())
        if _OP_NAMES.get(code) == "DEQUANTIZE":
            in_idx = op.Inputs(0)
            out_idx = op.Outputs(0)
            if in_idx in consts:
                consts[out_idx] = consts[in_idx].astype(np.float32)
                folded_ops.add(i)

    nodes: List[onnx.NodeProto] = []
    initializers: Dict[str, np.ndarray] = {}
    uid = 0

    def next_uid(prefix: str) -> str:
        nonlocal uid
        uid += 1
        return f"{prefix}_{uid}"

    def add_init(name: str, arr: np.ndarray) -> str:
        initializers[name] = np.ascontiguousarray(arr)
        return name

    # Determine graph inputs
    graph_inputs = []
    in_indices = [sg.Inputs(k) for k in range(sg.InputsLength())]
    for k, tidx in enumerate(in_indices):
        shape = list(t_shapes[tidx])
        ext_name = clean_input_names[k] if clean_input_names and k < len(clean_input_names) else t_names[tidx]
        if transpose_image_to_nchw and len(shape) == 4 and shape[3] in (1, 3, 4):
            n, h, w, c = shape
            nchw_shape = [n, c, h, w]
            t_names[tidx] = ext_name
            t_shapes[tidx] = nchw_shape
            t_is_nchw[tidx] = True
            graph_inputs.append(helper.make_tensor_value_info(ext_name, TensorProto.FLOAT, nchw_shape))
        else:
            t_names[tidx] = ext_name
            graph_inputs.append(helper.make_tensor_value_info(ext_name, TensorProto.FLOAT, shape))

    def ensure_const_name(tidx: int, desired_name: str | None = None) -> str:
        name = desired_name or t_names[tidx]
        if name not in initializers:
            add_init(name, consts[tidx].astype(np.float32) if consts[tidx].dtype == np.float16 else consts[tidx])
        return name

    def make_act(val_name: str, act_code: int, out_name: str) -> None:
        if act_code == 0:
            if val_name != out_name:
                nodes.append(helper.make_node("Identity", [val_name], [out_name]))
        elif act_code == 1:  # RELU
            nodes.append(helper.make_node("Relu", [val_name], [out_name]))
        elif act_code == 3:  # RELU6
            cmin = add_init(next_uid("relu6_min"), np.array(0.0, dtype=np.float32))
            cmax = add_init(next_uid("relu6_max"), np.array(6.0, dtype=np.float32))
            nodes.append(helper.make_node("Clip", [val_name, cmin, cmax], [out_name]))
        elif act_code == 2:  # RELU_N1_TO_1
            cmin = add_init(next_uid("relun1_min"), np.array(-1.0, dtype=np.float32))
            cmax = add_init(next_uid("relun1_max"), np.array(1.0, dtype=np.float32))
            nodes.append(helper.make_node("Clip", [val_name, cmin, cmax], [out_name]))
        else:
            raise NotImplementedError(f"Unsupported fused activation {act_code}")

    def as_nhwc_tensor(tidx: int) -> str:
        if not t_is_nchw[tidx]:
            return ensure_const_name(tidx) if tidx in consts else t_names[tidx]
        out_n = next_uid(t_names[tidx] + "_to_nhwc")
        nodes.append(helper.make_node("Transpose", [t_names[tidx]], [out_n], perm=[0, 2, 3, 1]))
        return out_n

    def as_nchw_tensor(tidx: int) -> str:
        if t_is_nchw[tidx]:
            return t_names[tidx]
        if tidx in consts:
            arr = consts[tidx]
            if arr.ndim == 4:
                n = next_uid(t_names[tidx] + "_nchw")
                return add_init(n, np.transpose(arr, (0, 3, 1, 2)))
            if arr.ndim == 1 and len(arr) > 1:
                n = next_uid(t_names[tidx] + "_c11")
                return add_init(n, arr.reshape(1, len(arr), 1, 1))
            if arr.ndim == 0 or arr.size == 1:
                n = next_uid(t_names[tidx] + "_scalar")
                return add_init(n, arr.reshape(()))
            return ensure_const_name(tidx)
        # Dynamic 4D NHWC tensor -> permute to NCHW
        shape = t_shapes[tidx]
        if len(shape) == 4:
            out_n = next_uid(t_names[tidx] + "_to_nchw")
            nodes.append(helper.make_node("Transpose", [t_names[tidx]], [out_n], perm=[0, 3, 1, 2]))
            return out_n
        return t_names[tidx]

    def get_spatial_hw(tidx: int) -> Tuple[int, int]:
        s = t_shapes[tidx]
        if t_is_nchw[tidx]:
            return s[2], s[3]
        return s[1], s[2]

    def set_out_shape_from_tflite(out_idx: int, is_nchw: bool) -> None:
        raw_s = [int(d) for d in sg.Tensors(out_idx).ShapeAsNumpy()]
        if batch_override is not None and len(raw_s) >= 1 and raw_s[0] == 1:
            raw_s[0] = batch_override
        if is_nchw and len(raw_s) == 4:
            t_shapes[out_idx] = [raw_s[0], raw_s[3], raw_s[1], raw_s[2]]
            t_is_nchw[out_idx] = True
        else:
            t_shapes[out_idx] = raw_s
            t_is_nchw[out_idx] = False

    # Process operators cleanly
    for i in range(sg.OperatorsLength()):
        if i in folded_ops:
            continue
        op = sg.Operators(i)
        oc = m.OperatorCodes(op.OpcodeIndex())
        code = max(oc.BuiltinCode(), oc.DeprecatedBuiltinCode())
        op_name = _OP_NAMES.get(code, f"OP_{code}")
        ins = [op.Inputs(k) for k in range(op.InputsLength())]
        outs = [op.Outputs(k) for k in range(op.OutputsLength())]
        out_idx = outs[0]
        out_name = t_names[out_idx]

        if op_name == "CONV_2D":
            opt = tflite.Conv2DOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            x_nchw = as_nchw_tensor(ins[0])
            w_raw = consts[ins[1]]  # [outC, kH, kW, inC]
            w_nchw = np.transpose(w_raw, (0, 3, 1, 2)).astype(np.float32)
            w_name = add_init(next_uid("conv_w"), w_nchw)
            conv_ins = [x_nchw, w_name]
            if len(ins) > 2 and ins[2] >= 0:
                b_raw = consts[ins[2]].astype(np.float32).reshape(-1)
                conv_ins.append(add_init(next_uid("conv_b"), b_raw))

            in_h, in_w = get_spatial_hw(ins[0])
            k_h, k_w = w_nchw.shape[2], w_nchw.shape[3]
            s_h, s_w = max(1, opt.StrideH()), max(1, opt.StrideW())
            d_h, d_w = max(1, opt.DilationHFactor()), max(1, opt.DilationWFactor())
            if opt.Padding() == 0:  # SAME
                pads = _same_padding_Explicit(in_h, in_w, k_h, k_w, s_h, s_w, d_h, d_w)
            else:
                pads = [0, 0, 0, 0]

            act = opt.FusedActivationFunction()
            conv_out = out_name if act == 0 else next_uid(out_name + "_preact")
            nodes.append(helper.make_node(
                "Conv",
                conv_ins,
                [conv_out],
                kernel_shape=[k_h, k_w],
                strides=[s_h, s_w],
                dilations=[d_h, d_w],
                pads=pads,
                group=1,
            ))
            if act != 0:
                make_act(conv_out, act, out_name)
            set_out_shape_from_tflite(out_idx, is_nchw=True)

        elif op_name == "DEPTHWISE_CONV_2D":
            opt = tflite.DepthwiseConv2DOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            x_nchw = as_nchw_tensor(ins[0])
            w_raw = consts[ins[1]]  # [1, kH, kW, outC]
            _, k_h, k_w, out_c = w_raw.shape
            dm = max(1, opt.DepthMultiplier())
            in_c = out_c // dm
            # ONNX Conv group=in_c takes weight [outC, inC/group (=1), kH, kW]
            w_nchw = np.transpose(w_raw.reshape(k_h, k_w, in_c, dm), (2, 3, 0, 1)).reshape(out_c, 1, k_h, k_w).astype(np.float32)
            w_name = add_init(next_uid("dw_w"), w_nchw)
            conv_ins = [x_nchw, w_name]
            if len(ins) > 2 and ins[2] >= 0:
                conv_ins.append(add_init(next_uid("dw_b"), consts[ins[2]].astype(np.float32).reshape(-1)))

            in_h, in_w = get_spatial_hw(ins[0])
            s_h, s_w = max(1, opt.StrideH()), max(1, opt.StrideW())
            d_h, d_w = max(1, opt.DilationHFactor()), max(1, opt.DilationWFactor())
            if opt.Padding() == 0:
                pads = _same_padding_Explicit(in_h, in_w, k_h, k_w, s_h, s_w, d_h, d_w)
            else:
                pads = [0, 0, 0, 0]

            act = opt.FusedActivationFunction()
            conv_out = out_name if act == 0 else next_uid(out_name + "_preact")
            nodes.append(helper.make_node(
                "Conv",
                conv_ins,
                [conv_out],
                kernel_shape=[k_h, k_w],
                strides=[s_h, s_w],
                dilations=[d_h, d_w],
                pads=pads,
                group=in_c,
            ))
            if act != 0:
                make_act(conv_out, act, out_name)
            set_out_shape_from_tflite(out_idx, is_nchw=True)

        elif op_name == "MAX_POOL_2D":
            opt = tflite.Pool2DOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            x_nchw = as_nchw_tensor(ins[0])
            in_h, in_w = get_spatial_hw(ins[0])
            k_h, k_w = opt.FilterHeight(), opt.FilterWidth()
            s_h, s_w = max(1, opt.StrideH()), max(1, opt.StrideW())
            pads = _same_padding_Explicit(in_h, in_w, k_h, k_w, s_h, s_w) if opt.Padding() == 0 else [0, 0, 0, 0]
            nodes.append(helper.make_node(
                "MaxPool",
                [x_nchw],
                [out_name],
                kernel_shape=[k_h, k_w],
                strides=[s_h, s_w],
                pads=pads,
            ))
            set_out_shape_from_tflite(out_idx, is_nchw=True)

        elif op_name == "PAD":
            p_raw = consts[ins[1]].astype(np.int64)  # [rank, 2]
            if t_is_nchw[ins[0]] and p_raw.shape == (4, 2):
                p_nchw = p_raw[[0, 3, 1, 2], :]
                onnx_pads = np.concatenate([p_nchw[:, 0], p_nchw[:, 1]]).astype(np.int64)
                x_n = t_names[ins[0]]
                is_nchw = True
            else:
                onnx_pads = np.concatenate([p_raw[:, 0], p_raw[:, 1]]).astype(np.int64)
                x_n = as_nhwc_tensor(ins[0]) if len(t_shapes[ins[0]]) == 4 else t_names[ins[0]]
                is_nchw = False
            pads_name = add_init(next_uid("pads"), onnx_pads)
            nodes.append(helper.make_node("Pad", [x_n, pads_name], [out_name], mode="constant"))
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw)

        elif op_name == "PRELU":
            x_nchw = as_nchw_tensor(ins[0])
            alpha = consts[ins[1]].astype(np.float32).squeeze()
            if alpha.ndim == 1:
                alpha = alpha.reshape(len(alpha), 1, 1)
            alpha_name = add_init(next_uid("prelu_a"), alpha)
            nodes.append(helper.make_node("PRelu", [x_nchw, alpha_name], [out_name]))
            set_out_shape_from_tflite(out_idx, is_nchw=True)

        elif op_name in ("RELU", "LOGISTIC", "NEG", "SQRT"):
            onnx_op = {"RELU": "Relu", "LOGISTIC": "Sigmoid", "NEG": "Neg", "SQRT": "Sqrt"}[op_name]
            is_nchw = t_is_nchw[ins[0]]
            nodes.append(helper.make_node(onnx_op, [t_names[ins[0]]], [out_name]))
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw)

        elif op_name == "RSQRT":
            is_nchw = t_is_nchw[ins[0]]
            sq_out = next_uid(out_name + "_sqrt")
            nodes.append(helper.make_node("Sqrt", [t_names[ins[0]]], [sq_out]))
            nodes.append(helper.make_node("Reciprocal", [sq_out], [out_name]))
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw)

        elif op_name in ("ADD", "SUB", "MUL", "DIV", "SQUARED_DIFFERENCE"):
            any_nchw = (t_is_nchw[ins[0]] and ins[0] not in consts) or (t_is_nchw[ins[1]] and ins[1] not in consts)
            if any_nchw:
                a_n = as_nchw_tensor(ins[0])
                b_n = as_nchw_tensor(ins[1])
                is_nchw = True
            else:
                a_n = ensure_const_name(ins[0]) if ins[0] in consts else as_nhwc_tensor(ins[0])
                b_n = ensure_const_name(ins[1]) if ins[1] in consts else as_nhwc_tensor(ins[1])
                is_nchw = False

            # Check fused activations on ADD/SUB/MUL/DIV
            act = 0
            if op_name == "ADD" and op.BuiltinOptions() is not None:
                o = tflite.AddOptions(); o.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
                act = o.FusedActivationFunction()
            elif op_name == "SUB" and op.BuiltinOptions() is not None:
                o = tflite.SubOptions(); o.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
                act = o.FusedActivationFunction()
            elif op_name == "MUL" and op.BuiltinOptions() is not None:
                o = tflite.MulOptions(); o.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
                act = o.FusedActivationFunction()
            elif op_name == "DIV" and op.BuiltinOptions() is not None:
                o = tflite.DivOptions(); o.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
                act = o.FusedActivationFunction()

            pre_out = out_name if act == 0 else next_uid(out_name + "_preact")
            if op_name == "SQUARED_DIFFERENCE":
                diff_out = next_uid(out_name + "_diff")
                nodes.append(helper.make_node("Sub", [a_n, b_n], [diff_out]))
                nodes.append(helper.make_node("Mul", [diff_out, diff_out], [pre_out]))
            else:
                onnx_op = {"ADD": "Add", "SUB": "Sub", "MUL": "Mul", "DIV": "Div"}[op_name]
                nodes.append(helper.make_node(onnx_op, [a_n, b_n], [pre_out]))
            if act != 0:
                make_act(pre_out, act, out_name)
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw)

        elif op_name == "CONCATENATION":
            opt = tflite.ConcatenationOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            axis = int(opt.Axis())
            rank = len(sg.Tensors(out_idx).ShapeAsNumpy())
            if axis < 0:
                axis += rank
            all_nchw_ok = rank == 4 and any(t_is_nchw[x] for x in ins if x not in consts)
            if all_nchw_ok:
                in_names = [as_nchw_tensor(x) for x in ins]
                axis_map = [0, 2, 3, 1]  # NHWC index j -> NCHW index axis_map[j]
                onnx_axis = axis_map[axis]
                is_nchw = True
            else:
                in_names = [ensure_const_name(x) if x in consts else as_nhwc_tensor(x) for x in ins]
                onnx_axis = axis
                is_nchw = False
            act = opt.FusedActivationFunction()
            pre_out = out_name if act == 0 else next_uid(out_name + "_preact")
            nodes.append(helper.make_node("Concat", in_names, [pre_out], axis=onnx_axis))
            if act != 0:
                make_act(pre_out, act, out_name)
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw)

        elif op_name == "RESIZE_BILINEAR":
            opt = tflite.ResizeBilinearOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            x_nchw = as_nchw_tensor(ins[0])
            new_hw = consts[ins[1]].astype(np.int64).reshape(-1)
            n = t_shapes[ins[0]][0]
            c = t_shapes[ins[0]][1] if t_is_nchw[ins[0]] else t_shapes[ins[0]][3]
            sizes_name = add_init(next_uid("resize_sz"), np.array([n, c, int(new_hw[0]), int(new_hw[1])], dtype=np.int64))
            roi_name = add_init(next_uid("resize_roi"), np.array([], dtype=np.float32))
            scales_name = add_init(next_uid("resize_sc"), np.array([], dtype=np.float32))
            ctm = "align_corners" if opt.AlignCorners() else ("half_pixel" if opt.HalfPixelCenters() else "asymmetric")
            nodes.append(helper.make_node(
                "Resize",
                [x_nchw, roi_name, scales_name, sizes_name],
                [out_name],
                mode="linear",
                coordinate_transformation_mode=ctm,
            ))
            set_out_shape_from_tflite(out_idx, is_nchw=True)

        elif op_name in ("MEAN", "SUM"):
            opt = tflite.ReducerOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            axes = [int(a) for a in consts[ins[1]].astype(np.int64).reshape(-1)]
            rank = len(sg.Tensors(ins[0]).ShapeAsNumpy())
            axes = [a + rank if a < 0 else a for a in axes]
            keepdims = 1 if opt.KeepDims() else 0
            out_rank = len(sg.Tensors(out_idx).ShapeAsNumpy())
            is_nchw_out = t_is_nchw[ins[0]] and keepdims == 1 and out_rank == 4
            if t_is_nchw[ins[0]] and rank == 4:
                axis_map = [0, 2, 3, 1]
                onnx_axes = [axis_map[a] for a in axes]
                x_n = t_names[ins[0]]
            else:
                x_n = as_nhwc_tensor(ins[0])
                onnx_axes = axes
            if op_name == "MEAN":
                nodes.append(helper.make_node("ReduceMean", [x_n], [out_name], axes=onnx_axes, keepdims=keepdims))
            else:
                axes_name = add_init(next_uid("red_axes"), np.array(onnx_axes, dtype=np.int64))
                nodes.append(helper.make_node("ReduceSum", [x_n, axes_name], [out_name], keepdims=keepdims))
            set_out_shape_from_tflite(out_idx, is_nchw=is_nchw_out)

        elif op_name == "RESHAPE":
            opt = tflite.ReshapeOptions()
            # Shape can be in ins[1] or options.NewShape
            if len(ins) > 1 and ins[1] >= 0 and ins[1] in consts:
                tgt = [int(d) for d in consts[ins[1]].astype(np.int64).reshape(-1)]
            else:
                opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
                tgt = [int(opt.NewShape(k)) for k in range(opt.NewShapeLength())]
            if batch_override is not None and len(tgt) >= 1 and tgt[0] == 1:
                tgt[0] = batch_override
            # Crucial: if input was NCHW, transpose back to NHWC before reshape so flat order matches TFLite!
            x_n = as_nhwc_tensor(ins[0])
            # Also fix -1 inference if batch_override was set: compute product carefully when safe
            tgt_arr = np.array(tgt, dtype=np.int64)
            shape_name = add_init(next_uid("reshape_tgt"), tgt_arr)
            nodes.append(helper.make_node("Reshape", [x_n, shape_name], [out_name]))
            set_out_shape_from_tflite(out_idx, is_nchw=False)

        elif op_name == "TRANSPOSE":
            perm = [int(p) for p in consts[ins[1]].astype(np.int64).reshape(-1)]
            x_n = as_nhwc_tensor(ins[0])
            nodes.append(helper.make_node("Transpose", [x_n], [out_name], perm=perm))
            set_out_shape_from_tflite(out_idx, is_nchw=False)

        elif op_name == "STRIDED_SLICE":
            opt = tflite.StridedSliceOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            in_shape = t_shapes[ins[0]]
            rank = len(in_shape)
            begins = consts[ins[1]].astype(np.int64).reshape(-1).tolist()
            ends = consts[ins[2]].astype(np.int64).reshape(-1).tolist()
            strides = consts[ins[3]].astype(np.int64).reshape(-1).tolist()

            starts_out, ends_out, axes_out, steps_out = [], [], [], []
            squeeze_axes = []
            for d in range(rank):
                s = strides[d]
                b = 0 if (opt.BeginMask() & (1 << d)) else begins[d]
                if opt.ShrinkAxisMask() & (1 << d):
                    b_eff = begins[d] if begins[d] >= 0 else (begins[d] + in_shape[d])
                    e_eff = b_eff + 1
                    squeeze_axes.append(d)
                    starts_out.append(b_eff)
                    ends_out.append(e_eff)
                    axes_out.append(d)
                    steps_out.append(1)
                    continue
                e = in_shape[d] if (opt.EndMask() & (1 << d)) else ends[d]
                starts_out.append(b)
                ends_out.append(e)
                axes_out.append(d)
                steps_out.append(s)

            x_n = as_nhwc_tensor(ins[0]) if ins[0] not in consts else ensure_const_name(ins[0])
            st_n = add_init(next_uid("sl_starts"), np.array(starts_out, dtype=np.int64))
            en_n = add_init(next_uid("sl_ends"), np.array(ends_out, dtype=np.int64))
            ax_n = add_init(next_uid("sl_axes"), np.array(axes_out, dtype=np.int64))
            sp_n = add_init(next_uid("sl_steps"), np.array(steps_out, dtype=np.int64))
            slice_out = out_name if not squeeze_axes else next_uid(out_name + "_presq")
            nodes.append(helper.make_node("Slice", [x_n, st_n, en_n, ax_n, sp_n], [slice_out]))
            if squeeze_axes:
                sq_ax = add_init(next_uid("sq_axes"), np.array(squeeze_axes, dtype=np.int64))
                nodes.append(helper.make_node("Squeeze", [slice_out, sq_ax], [out_name]))
            set_out_shape_from_tflite(out_idx, is_nchw=False)

        elif op_name == "FULLY_CONNECTED":
            opt = tflite.FullyConnectedOptions()
            opt.Init(op.BuiltinOptions().Bytes, op.BuiltinOptions().Pos)
            x_n = as_nhwc_tensor(ins[0])
            w_raw = consts[ins[1]].astype(np.float32)  # [outF, inF]
            in_shape = t_shapes[ins[0]]
            if len(in_shape) > 2 or in_shape[-1] != w_raw.shape[1]:
                fl_out = next_uid("fc_flat")
                sh_n = add_init(next_uid("fc_sh"), np.array([-1, w_raw.shape[1]], dtype=np.int64))
                nodes.append(helper.make_node("Reshape", [x_n, sh_n], [fl_out]))
                x_n = fl_out
            w_n = add_init(next_uid("fc_w"), w_raw)
            gemm_ins = [x_n, w_n]
            if len(ins) > 2 and ins[2] >= 0:
                gemm_ins.append(add_init(next_uid("fc_b"), consts[ins[2]].astype(np.float32).reshape(-1)))
            act = opt.FusedActivationFunction()
            pre_out = out_name if act == 0 else next_uid(out_name + "_preact")
            nodes.append(helper.make_node("Gemm", gemm_ins, [pre_out], transB=1))
            if act != 0:
                make_act(pre_out, act, out_name)
            set_out_shape_from_tflite(out_idx, is_nchw=False)

        else:
            raise NotImplementedError(f"Unsupported TFLite op: {op_name}")

    # Collect graph outputs (ensure NHWC order cleanly on 4D if model expected NHWC, OR normalize clean static 2D/3D)
    graph_outputs = []
    out_indices = [sg.Outputs(k) for k in range(sg.OutputsLength())]
    for k, oidx in enumerate(out_indices):
        ext_out = clean_output_names[k] if clean_output_names and k < len(clean_output_names) else t_names[oidx]
        cur_name = t_names[oidx]
        if t_is_nchw[oidx] and len(t_shapes[oidx]) == 4:
            raw_s = [int(d) for d in sg.Tensors(oidx).ShapeAsNumpy()]
            if batch_override is not None and len(raw_s) >= 1 and raw_s[0] == 1:
                raw_s[0] = batch_override
            nodes.append(helper.make_node("Transpose", [cur_name], [ext_out], perm=[0, 2, 3, 1]))
            out_shape = raw_s
        else:
            if cur_name != ext_out:
                nodes.append(helper.make_node("Identity", [cur_name], [ext_out]))
            out_shape = list(t_shapes[oidx])
        graph_outputs.append(helper.make_tensor_value_info(ext_out, TensorProto.FLOAT, out_shape))

    init_protos = [numpy_helper.from_array(arr, name=name) for name, arr in initializers.items()]
    graph_def = helper.make_graph(nodes, model_name, graph_inputs, graph_outputs, initializer=init_protos)
    model_def = helper.make_model(graph_def, opset_imports=[helper.make_opsetid("", 17)], ir_version=8)
    onnx.checker.check_model(model_def)
    onnx.save(model_def, out_onnx_path)
    return model_def
