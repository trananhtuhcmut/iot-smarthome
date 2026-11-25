import os
from ultralytics import YOLO

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_YAML = os.path.join(BASE_DIR, "datasets", "fire_smoke", "data.yaml")
BASE_WEIGHT = os.path.join(BASE_DIR, "weights", "yolo11n.pt")


def main():
    # Kiểm tra file data.yaml trước khi train
    if not os.path.exists(DATA_YAML):
        print(f"Không tìm thấy {DATA_YAML}. Hãy giải nén dataset từ Roboflow vào đúng thư mục!")
        return

    model = YOLO(BASE_WEIGHT)

    # Cấu hình tối ưu cho RTX 4050 Laptop (6GB VRAM)
    model.train(
        data=DATA_YAML,
        epochs=50,          # Số vòng học toàn bộ dataset (50 là vừa đẹp cho Transfer Learning)
        imgsz=640,          # Kích thước ảnh đầu vào khớp lúc inference
        batch=16,           # Vừa vặn 6GB VRAM (nếu báo Out of Memory thì hạ xuống 8)
        device=0,           # Ép chạy trên GPU RTX 4050 (index 0)
        workers=4,          # Số luồng CPU đọc ảnh nạp cho GPU
        amp=True,           # Automatic Mixed Precision (FP16) tăng tốc trên Tensor Core
        patience=15,        # Tự dừng sớm (Early Stopping) nếu 15 epoch không tăng mAP nữa
        project=os.path.join(BASE_DIR, "runs"),
        name="fire_yolo11n",
    )


# BẮT BUỘC có khối if __name__ == "__main__": trên Windows khi dùng workers > 0!
if __name__ == "__main__":
    main()