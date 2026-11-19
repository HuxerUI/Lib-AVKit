# YuNet 2023mar

This pack contains the FP32 ncnn conversion of OpenCV Zoo's `face_detection_yunet_2023mar.onnx`. It uses one BGR input at 640 × 640 and the twelve YuNet output tensors. The original ONNX model has SHA-256 `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`. [Upstream model and license](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)

Verify the pack with `shasum -a 256 -c SHA256SUMS`. To reproduce the conversion, use PNNX 20261020 and run `pnnx yunet.onnx fp16=0` on the original model renamed to `yunet.onnx`. Distribute `LICENSE` with the generated graph and weights. [PNNX](https://pypi.org/project/pnnx/20261020/)

Pass the two verified assets as `RawAsset` values to `CreateNcnnFaceAnalysisProvider`. Applications can use HuxerUI resources or provide memory-backed assets from their own model catalog.
