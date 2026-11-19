# PP-HumanSegV2-Lite for ncnn

This pack converts PaddleSeg's 192×192 general human segmentation model with softmax output. It provides foreground coverage for wide shots and full-body subjects. The graph accepts 256×256 planar RGB in `[0, 1]`, resizes and normalizes it for the PaddleSeg model, and returns the foreground probability on the 256×256 grid. The stable model ID is `pp-humansegv2-lite-192-v1`. The upstream Apache 2.0 license accompanies the generated files. [PaddleSeg model](https://github.com/PaddlePaddle/PaddleSeg/blob/release/2.10/contrib/PP-HumanSeg/README.md#42-general-human-segmentation-models)

Use Python 3.11 and PNNX 20261020.

```sh
python3.11 -m pip install paddle2onnx==1.3.1 onnx==1.19.1
python3.11 models/paddle/pp-humansegv2-lite/prepare.py /path/to/pnnx build/wide-portrait-model
(cd build/wide-portrait-model && shasum -a 256 -c SHA256SUMS)
```

The recipe verifies the original PaddleSeg download and the resulting ncnn assets. It lowers the two adaptive average pools to the equivalent fixed-size operations for this input size. Pass the graph and weights as `NcnnAnalysisModels::portrait_wide`. `NcnnAnalysisModels::portrait` supplies the default MediaPipe model. The macOS example finds both sets of files when placed in the same model directory.
