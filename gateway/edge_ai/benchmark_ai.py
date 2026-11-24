import glob
import os
import time
import cv2
import torch
from ultralytics import YOLO

try:
    import pynvml
    PYNVML_OK = True
except ImportError:
    PYNVML_OK = False

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PERSON_WEIGHT = os.path.join(BASE_DIR, "weights", "yolo11n.pt")
FIRE_WEIGHT   = os.path.join(BASE_DIR, "weights", "fire_best.pt")
VALID_IMG_DIR = os.path.join(BASE_DIR, "datasets", "fire_smoke", "valid", "images")

NUM_TEST_IMAGES = 100  # Số ảnh dùng đo trung bình
WARMUP_FRAMES   = 10   # Số ảnh làm nóng CPU/GPU trước khi bấm giờ


def load_test_frames(limit: int):
    """Đọc sẵn tối đa `limit` ảnh vào RAM để loại bỏ độ trễ đọc ổ cứng khi đo."""
    paths = sorted(glob.glob(os.path.join(VALID_IMG_DIR, "*.*")))[:limit]
    frames = []
    for p in paths:
        img = cv2.imread(p)
        if img is not None:
            frames.append(cv2.resize(img, (640, 480)))
    return frames


def get_gpu_util(handle):
    if not handle:
        return 0.0
    try:
        return float(pynvml.nvmlDeviceGetUtilizationRates(handle).gpu)
    except Exception:
        return 0.0


def run_benchmark(device: str, frames: list):
    use_cuda = device.startswith("cuda")
    use_half = use_cuda

    person_model = YOLO(PERSON_WEIGHT)
    fire_model = YOLO(FIRE_WEIGHT) if os.path.exists(FIRE_WEIGHT) else None
    fire_ids = []
    if fire_model is not None:
        fire_ids = [
            cid for cid, cname in fire_model.names.items()
            if any(k in str(cname).lower() for k in ("fire", "smoke", "flame"))
        ]

    # Khởi tạo theo dõi % GPU nếu chạy CUDA
    nvml_handle = None
    if use_cuda and PYNVML_OK:
        try:
            pynvml.nvmlInit()
            nvml_handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        except Exception:
            pass

    print(f"\n>>> Đang chạy Benchmark trên [{device.upper()}] (FP16={use_half}, {len(frames)} ảnh)...")

    # 1. Warm-up (không tính giờ)
    for i in range(min(WARMUP_FRAMES, len(frames))):
        person_model.predict(frames[i], classes=[0], conf=0.60, imgsz=640, device=device, half=use_half, verbose=False)
        if fire_model and fire_ids:
            fire_model.predict(frames[i], classes=fire_ids, conf=0.45, imgsz=640, device=device, half=use_half, verbose=False)
    if use_cuda:
        torch.cuda.synchronize()

    # 2. Đo thời gian thực tế
    latencies_ms = []
    gpu_samples = []

    for frame in frames:
        if use_cuda:
            torch.cuda.synchronize()
        t0 = time.perf_counter()

        person_model.predict(frame, classes=[0], conf=0.60, imgsz=640, device=device, half=use_half, verbose=False)
        if fire_model and fire_ids:
            fire_model.predict(frame, classes=fire_ids, conf=0.45, imgsz=640, device=device, half=use_half, verbose=False)

        if use_cuda:
            torch.cuda.synchronize()  # Bắt buộc chờ GPU tính xong hẳn mới dừng đồng hồ
        t1 = time.perf_counter()

        latencies_ms.append((t1 - t0) * 1000.0)
        if nvml_handle:
            gpu_samples.append(get_gpu_util(nvml_handle))

    if nvml_handle:
        pynvml.nvmlShutdown()

    avg_ms = sum(latencies_ms) / len(latencies_ms)
    min_ms = min(latencies_ms)
    max_ms = max(latencies_ms)
    fps = 1000.0 / avg_ms
    avg_gpu = (sum(gpu_samples) / len(gpu_samples)) if gpu_samples else 0.0

    return {
        "device": device.upper(),
        "precision": "FP16" if use_half else "FP32",
        "avg_ms": avg_ms,
        "min_ms": min_ms,
        "max_ms": max_ms,
        "fps": fps,
        "gpu_util": avg_gpu,
    }


def main():
    frames = load_test_frames(NUM_TEST_IMAGES)
    if not frames:
        print(f"Không tìm thấy ảnh test tại: {VALID_IMG_DIR}")
        return

    print(f"Đã nạp sẵn {len(frames)} ảnh 640x480 vào RAM.")
    res_cpu = run_benchmark("cpu", frames)

    res_gpu = None
    if torch.cuda.is_available():
        gpu_name = torch.cuda.get_device_name(0)
        res_gpu = run_benchmark("cuda", frames)
    else:
        gpu_name = "Không có CUDA"

    print("\n" + "=" * 78)
    print(f"KẾT QUẢ BENCHMARK DUAL-MODEL (Person + Fire/Smoke) | GPU: {gpu_name}")
    print("=" * 78)
    print(f"| {'Thiết bị':<10} | {'Độ chính xác':<12} | {'Avg (ms)':<10} | {'Min/Max (ms)':<15} | {'AI FPS':<10} | {'GPU %':<7} |")
    print(f"|{'-'*12}|{'-'*14}|{'-'*12}|{'-'*17}|{'-'*12}|{'-'*9}|")
    for r in [res_cpu, res_gpu]:
        if not r:
            continue
        mm = f"{r['min_ms']:.1f} / {r['max_ms']:.1f}"
        print(f"| {r['device']:<10} | {r['precision']:<12} | {r['avg_ms']:<10.2f} | {mm:<15} | {r['fps']:<10.1f} | {r['gpu_util']:<7.1f} |")

    if res_gpu:
        speedup = res_cpu["avg_ms"] / res_gpu["avg_ms"]
        print(f"\n=> Hệ số tăng tốc GPU (Speedup): {speedup:.2f}x nhanh hơn so với CPU!")
    print("=" * 78)


if __name__ == "__main__":
    main()