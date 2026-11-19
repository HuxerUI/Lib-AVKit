# MediaPipe body and hand models for ncnn

This pack uses the OpenCV Zoo exports of MediaPipe person detection, pose estimation, palm detection, and hand pose estimation. The pack contains their FP32 ncnn conversions and uses the same ncnn runtime as YuNet. The Apache 2.0 license accompanies the converted files. [Pinned OpenCV Zoo source](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models)

```sh
python3 models/mediapipe/body-hand/prepare.py /path/to/pnnx build/body-hand-models
(cd build/body-hand-models && shasum -a 256 -c SHA256SUMS)
```

Use PNNX 20261020. The recipe verifies the original ONNX files, translates the required padding and pixel shuffle operators, and verifies the converted graph and weight hashes. The weight files are generated separately. Pass the resulting detector and landmark pairs to `CreateNcnnAnalysisProvider` as `RawAsset` values. The macOS preview can load the model directory directly. [PNNX 20261020](https://pypi.org/project/pnnx/20261020/)
