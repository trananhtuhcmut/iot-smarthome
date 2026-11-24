import sqlite3
import threading
import time
from typing import List, Tuple


class OfflineBufferDB:
    """
    Quản lý hàng đợi lưu đệm (Store-and-Forward) bằng SQLite.
    Đảm bảo dữ liệu không bị mất khi mất Internet và được đẩy lên CoreIOT theo thứ tự FIFO.
    """

    def __init__(self, db_path: str = "gateway_buffer.db"):
        self.db_path = db_path
        # Lock giúp đồng bộ giữa các luồng (Thread-safe), tránh xung đột ghi/đọc cùng lúc
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(self.db_path, check_same_thread=False)
        self._init_db()

    def _init_db(self):
        with self._lock:
            cur = self._conn.cursor()
            # Bật WAL mode để đọc/ghi song song mượt hơn và hạn chế lỗi "database is locked"
            cur.execute("PRAGMA journal_mode=WAL;")
            # Bảng hàng đợi gửi lên CoreIOT (v1/gateway/telemetry)
            cur.execute("""
                CREATE TABLE IF NOT EXISTS cloud_queue (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    payload_json TEXT NOT NULL,
                    created_at INTEGER NOT NULL
                )
            """)
            # Bảng lưu lịch sử sự kiện/cảnh báo phục vụ LAN Web UI (Giai đoạn 3) & Báo cáo
            cur.execute("""
                CREATE TABLE IF NOT EXISTS event_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    ts_ms INTEGER NOT NULL,
                    source TEXT NOT NULL,
                    event_type TEXT NOT NULL,
                    detail TEXT NOT NULL
                )
            """)
            self._conn.commit()

    def enqueue_telemetry(self, payload_json: str) -> int:
        """Thêm 1 gói JSON chuẩn CoreIOT vào cuối hàng đợi, trả về tổng số gói đang chờ."""
        now_ms = int(time.time() * 1000)
        with self._lock:
            cur = self._conn.cursor()
            cur.execute(
                "INSERT INTO cloud_queue (payload_json, created_at) VALUES (?, ?)",
                (payload_json, now_ms),
            )
            self._conn.commit()
            cur.execute("SELECT COUNT(*) FROM cloud_queue")
            return cur.fetchone()[0]

    def peek_batch(self, limit: int = 10) -> List[Tuple[int, str]]:
        """Lấy tối đa `limit` gói tin CŨ NHẤT theo thứ tự FIFO (chưa xóa ngay)."""
        with self._lock:
            cur = self._conn.cursor()
            cur.execute(
                "SELECT id, payload_json FROM cloud_queue ORDER BY id ASC LIMIT ?",
                (limit,),
            )
            return cur.fetchall()

    def delete_by_id(self, row_id: int) -> None:
        """Chỉ xóa gói tin khỏi SQLite sau khi CoreIOT đã xác nhận PUBACK."""
        with self._lock:
            cur = self._conn.cursor()
            cur.execute("DELETE FROM cloud_queue WHERE id = ?", (row_id,))
            self._conn.commit()

    def count_queue(self) -> int:
        """Đếm số gói tin đang tồn đọng trong hàng đợi."""
        with self._lock:
            cur = self._conn.cursor()
            cur.execute("SELECT COUNT(*) FROM cloud_queue")
            return cur.fetchone()[0]

    def log_event(self, source: str, event_type: str, detail: str) -> None:
        """Ghi nhật ký hoạt động (đổi công tắc, cảnh báo AI, rớt mạng/có mạng)."""
        now_ms = int(time.time() * 1000)
        with self._lock:
            cur = self._conn.cursor()
            cur.execute(
                "INSERT INTO event_logs (ts_ms, source, event_type, detail) VALUES (?, ?, ?, ?)",
                (now_ms, source, event_type, detail),
            )
            self._conn.commit()

    def close(self):
        with self._lock:
            self._conn.close()