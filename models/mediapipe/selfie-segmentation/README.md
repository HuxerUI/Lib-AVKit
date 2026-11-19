# MediaPipe Selfie Segmentation for ncnn

This pack converts the MediaPipe square Selfie Segmentation graph distributed in Qualcomm AI Hub Models' `v0.67.0` float ONNX release. The input is planar RGB in `[0, 1]` at 256×256. The single sigmoid output contains foreground probabilities. AVKit keeps the fractional coverage when converting the output to `R8Unorm`. The original model uses the accompanying Apache 2.0 license. [MediaPipe model](https://github.com/google-ai-edge/mediapipe/blob/master/docs/solutions/selfie_segmentation.md) · [Pinned ONNX release manifest](https://huggingface.co/qualcomm/MediaPipe-Selfie-Segmentation/blob/3937e5876bb68a5e1d91751eb36078677eb02a9a/release_assets.json)

```sh
python3 -m pip install onnx==1.22.0
python3 models/mediapipe/selfie-segmentation/prepare.py /path/to/pnnx build/selfie-models
(cd build/selfie-models && shasum -a 256 -c SHA256SUMS)
```

Use PNNX 20261020. The recipe checks the downloaded archive, embeds its external weights into the ONNX graph for conversion, and checks both ncnn outputs. Pass the generated `selfie.ncnn.param` and `selfie.ncnn.bin` as `NcnnAnalysisModels::portrait`. The example loader can discover this pack alongside the face and body packs in the same directory. [PNNX release](https://pypi.org/project/pnnx/20261020/)
