"""Download the pinned OpenCV Zoo ONNX models and prepare their ncnn pack."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

REVISION = "48b10fa505aeb51fbd127fc60a5c660040e823b7"
MODELS = {
    "mp_persondet": (
        "person_detection_mediapipe", "2023mar",
        "47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f",
    ),
    "mp_pose": (
        "pose_estimation_mediapipe", "2023mar",
        "9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f",
    ),
    "mp_palmdet": (
        "palm_detection_mediapipe", "2023feb",
        "78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c",
    ),
    "mp_handpose": (
        "handpose_estimation_mediapipe", "2023feb",
        "db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c",
    ),
}
PALM_CHANNEL_PADDING = {"Pad_16": 32, "Pad_34": 64, "Pad_52": 128}


def verify(path, expected):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f"{path.name} has SHA-256 {actual}, expected {expected}")


def lower_parameters(path, model):
    lines = path.read_text().splitlines()
    for i, line in enumerate(lines[2:], 2):
        fields = line.split()
        if fields[0] == "DepthToSpace":
            # ONNX DCR ordering is mode 1 in ncnn.
            fields[0] = "PixelShuffle"
            fields.extend(("0=2", "1=1"))
        if fields[0] == "Pad":
            fields[0] = "Padding"
            if model == "mp_palmdet":
                fields.append(f"8={PALM_CHANNEL_PADDING[fields[1]]}")
            else:
                fields.extend(("0=1", "1=1", "2=1", "3=1"))
        # AVKit supplies planar RGB for the graphs originally exported with NHWC input.
        if model != "mp_persondet" and i == (4 if model == "mp_pose" else 3):
            if fields[0] != "Permute" or fields[-1] != "0=4":
                raise RuntimeError(f"Unexpected layout conversion in {model}")
            fields[0] = "Split"
            fields.pop()
        lines[i] = " ".join(fields)
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pnnx", type=Path, help="PNNX 20261020 executable")
    parser.add_argument("output", type=Path, help="Directory for the verified model assets")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    outputs = [line.split() for line in (source / "SHA256SUMS").read_text().splitlines()]
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        for stem, (folder, version, digest) in MODELS.items():
            onnx = work / f"{stem}.onnx"
            url = f"https://media.githubusercontent.com/media/opencv/opencv_zoo/{REVISION}/models/{folder}/{folder}_{version}.onnx"
            urllib.request.urlretrieve(url, onnx)
            verify(onnx, digest)
            subprocess.run([str(args.pnnx.resolve()), onnx.name, "fp16=0"], cwd=work, check=True)
            lower_parameters(work / f"{stem}.ncnn.param", stem)
        for digest, name in outputs:
            verify(work / name, digest)
        args.output.mkdir(parents=True, exist_ok=True)
        for _, name in outputs:
            shutil.copyfile(work / name, args.output / name)
        for name in ("SHA256SUMS", "LICENSE"):
            shutil.copyfile(source / name, args.output / name)


if __name__ == "__main__":
    main()
