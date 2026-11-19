"""Prepare the pinned general PP-HumanSegV2-Lite ncnn model pack."""

import argparse
import hashlib
from importlib.metadata import distribution
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from zipfile import ZipFile

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

MODEL = "human_pp_humansegv2_lite_192x192_inference_model_with_softmax"
URL = f"https://paddleseg.bj.bcebos.com/dygraph/pp_humanseg_v2/{MODEL}.zip"
ARCHIVE_SHA256 = "9d86bc9c86c282cc2e92eaa089b96e18d30776c6ca745be18e6251a4e2a00110"
ONNX_SHA256 = "3e3b5ee7a9428652b92c2a903dde6d7693c13119de5ef25d7383d56494c44cb0"
STEM = "pp_humansegv2_lite"


def verify(path, expected):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f"{path.name} has SHA-256 {actual}, expected {expected}")


def export_onnx(work):
    converter = distribution("paddle2onnx")
    if converter.version != "1.3.1":
        raise RuntimeError("This recipe uses Paddle2ONNX 1.3.1")
    # The wheel can convert the legacy deployment files directly through its extension.
    sys.path.insert(0, str(converter.locate_file("paddle2onnx")))
    import paddle2onnx_cpp2py_export as paddle2onnx

    archive = work / "paddle.zip"
    urllib.request.urlretrieve(URL, archive)
    verify(archive, ARCHIVE_SHA256)
    with ZipFile(archive) as files:
        for name in ("model.pdmodel", "model.pdiparams"):
            (work / name).write_bytes(files.read(f"{MODEL}/{name}"))
    result = paddle2onnx.export(str(work / "model.pdmodel"), str(work / "model.pdiparams"), 11,
                               True, True, True, True, True, {}, "onnxruntime", "", "", False)
    path = work / "paddle.onnx"
    path.write_bytes(result)
    verify(path, ONNX_SHA256)
    return onnx.load(path)


def adapt_graph(model):
    graph = model.graph
    original_input = graph.input[0].name
    original_output = graph.output[0].name

    def constant(name, value, dtype=np.int64):
        graph.initializer.append(numpy_helper.from_array(np.asarray(value, dtype=dtype), name))
        return name

    def crop(name, image, axis, start, end):
        args = [constant(name + suffix, [value]) for suffix, value in
                (("_start", start), ("_stop", end), ("_axis", axis))]
        return helper.make_node("Slice", [image, *args], [name], name=name)

    def resize(image, name, shape):
        return helper.make_node("Resize", [image, "", "", constant(name + "_size", shape)], [name], name=name,
                                mode="linear", coordinate_transformation_mode="half_pixel")

    nodes = [resize("rgb_256", "rgb_192", [1, 3, 192, 192]),
             helper.make_node("Mul", ["rgb_192", constant("scale_input", 2, np.float32)], ["scaled_rgb"]),
             helper.make_node("Sub", ["scaled_rgb", constant("offset_input", 1, np.float32)], [original_input])]
    pools = {"pool2d_9.tmp_0": 2, "pool2d_10.tmp_0": 4}
    for node in graph.node:
        if node.op_type != "AveragePool":
            nodes.append(node)
            continue
        name = node.output[0]
        if list(node.input) != ["Add.51"] or name not in pools:
            raise RuntimeError("Unexpected adaptive pool in the PP-HumanSeg graph")
        target = pools.pop(name)
        # The 2x2 output uses nonoverlapping 3x3 windows on the 6x6 encoder features.
        if target == 2:
            nodes.append(helper.make_node("AveragePool", node.input, [name], name=name,
                                          kernel_shape=[3, 3], strides=[3, 3]))
            continue
        # The 4x4 output uses 2x2 windows starting at 0, 1, 3, and 4 on each axis.
        pooled = name + "_pooled_5"
        nodes.append(helper.make_node("AveragePool", node.input, [pooled], name=pooled,
                                      kernel_shape=[2, 2], strides=[1, 1]))
        top, bottom = crop(name + "_top", pooled, 2, 0, 2), crop(name + "_bottom", pooled, 2, 3, 5)
        rows = name + "_rows"
        left, right = crop(name + "_left", rows, 3, 0, 2), crop(name + "_right", rows, 3, 3, 5)
        nodes.extend([top, bottom, helper.make_node("Concat", [top.output[0], bottom.output[0]], [rows], axis=2),
                      left, right, helper.make_node("Concat", [left.output[0], right.output[0]], [name], axis=3)])
    if pools:
        raise RuntimeError("Missing adaptive pools in the PP-HumanSeg graph")

    nodes.extend([crop("person_192", original_output, 1, 1, 2), resize("person_192", "mask_256", [1, 1, 256, 256])])
    graph.ClearField("node")
    graph.node.extend(nodes)
    graph.ClearField("input")
    graph.input.append(helper.make_tensor_value_info("rgb_256", TensorProto.FLOAT, [1, 3, 256, 256]))
    graph.ClearField("output")
    graph.output.append(helper.make_tensor_value_info("mask_256", TensorProto.FLOAT, [1, 1, 256, 256]))
    model = onnx.shape_inference.infer_shapes(model, strict_mode=True, data_prop=True)
    onnx.checker.check_model(model)
    return model


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pnnx", type=Path, help="PNNX 20261020 executable")
    parser.add_argument("output", type=Path, help="Directory for the verified model assets")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    outputs = [line.split() for line in (source / "SHA256SUMS").read_text().splitlines()]
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        graph = work / f"{STEM}.onnx"
        onnx.save(adapt_graph(export_onnx(work)), graph)
        subprocess.run([str(args.pnnx.resolve()), graph.name, "fp16=0"], cwd=work, check=True)
        for digest, name in outputs:
            verify(work / name, digest)
        args.output.mkdir(parents=True, exist_ok=True)
        for _, name in outputs:
            shutil.copyfile(work / name, args.output / name)
        for name in ("SHA256SUMS", "LICENSE"):
            shutil.copyfile(source / name, args.output / name)


if __name__ == "__main__":
    main()
