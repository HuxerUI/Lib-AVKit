"""Prepare the pinned MediaPipe Selfie Segmentation ncnn pack."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
from zipfile import ZipFile

import onnx

URL = ("https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/mediapipe_selfie/"
       "releases/v0.67.0/mediapipe_selfie-onnx-float.zip")
ARCHIVE_SHA256 = "8f51ea5cb140751e2b36c376af38c526122044e51e95e28f328f6ef7d6f85e3d"


def verify(path, expected):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f"{path.name} has SHA-256 {actual}, expected {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pnnx", type=Path, help="PNNX 20261020 executable")
    parser.add_argument("output", type=Path, help="Directory for the verified model assets")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    outputs = [line.split() for line in (source / "SHA256SUMS").read_text().splitlines()]
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        archive = work / "selfie.zip"
        urllib.request.urlretrieve(URL, archive)
        verify(archive, ARCHIVE_SHA256)
        prefix = "mediapipe_selfie-onnx-float/mediapipe_selfie_onnx/"
        with ZipFile(archive) as files:
            for name in ("mediapipe_selfie.onnx", "mediapipe_selfie.data"):
                (work / name).write_bytes(files.read(prefix + name))
        onnx.save(onnx.load(work / "mediapipe_selfie.onnx"), work / "selfie.onnx")
        subprocess.run([str(args.pnnx.resolve()), "selfie.onnx", "fp16=0"], cwd=work, check=True)
        for digest, name in outputs:
            verify(work / name, digest)
        args.output.mkdir(parents=True, exist_ok=True)
        for _, name in outputs:
            shutil.copyfile(work / name, args.output / name)
        for name in ("SHA256SUMS", "LICENSE"):
            shutil.copyfile(source / name, args.output / name)


if __name__ == "__main__":
    main()
