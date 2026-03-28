"""
Simulates ESP32-CAM POSTing a violation to the Flask server.
Usage:
  python simulate_esp32.py --image ../test_images/no_helmet_with_plate.jpg
  python simulate_esp32.py --loop --delay 5
  python simulate_esp32.py --loop --random --delay 3
"""
import argparse, os, random, time, io
import requests
from PIL import Image

SERVER_URL = os.getenv("SERVER_URL", "http://localhost:5000")
IMAGE_DIR  = os.path.join(os.path.dirname(__file__), '..', 'test_images')
SUPPORTED  = ('.jpg', '.jpeg', '.png', '.webp')

def get_all_images():
    return [
        os.path.join(IMAGE_DIR, f)
        for f in os.listdir(IMAGE_DIR)
        if f.lower().endswith(SUPPORTED)
    ]

def crop_and_send(image_path: str, confidence: float = 0.91):
    img = Image.open(image_path).convert("RGB")
    w, h = img.size
    plate_crop = img.crop((0, h // 2, w, h))   # bottom half = plate zone

    buf = io.BytesIO()
    plate_crop.save(buf, format="JPEG", quality=85)
    buf.seek(0)

    try:
        resp = requests.post(
            f"{SERVER_URL}/violation",
            files={"plate_crop": ("plate.jpg", buf, "image/jpeg")},
            data={"confidence": str(confidence)},
            timeout=20
        )
        fname = os.path.basename(image_path)
        if resp.status_code == 200:
            d = resp.json()
            print(f"[OK]  {fname:<35} plate={d['plate']}  ts={d['timestamp']}")
        else:
            print(f"[ERR] {fname:<35} status={resp.status_code} body={resp.text[:80]}")
    except requests.exceptions.ConnectionError:
        print(f"[ERR] Cannot connect to {SERVER_URL} — is Flask running?")
    except requests.exceptions.Timeout:
        print(f"[ERR] Request timed out")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--image",      default=None)
    parser.add_argument("--confidence", type=float, default=0.91)
    parser.add_argument("--loop",       action="store_true")
    parser.add_argument("--delay",      type=float, default=5.0)
    parser.add_argument("--random",     action="store_true")
    args = parser.parse_args()

    if args.image:
        crop_and_send(args.image, args.confidence)

    elif args.loop:
        images = get_all_images()
        if not images:
            print(f"No images found in {IMAGE_DIR}")
            exit(1)
        print(f"Found {len(images)} image(s). Sending every {args.delay}s. Ctrl+C to stop.\n")
        if args.random:
            random.shuffle(images)
        idx = 0
        while True:
            crop_and_send(images[idx % len(images)], args.confidence)
            idx += 1
            if idx % len(images) == 0 and args.random:
                random.shuffle(images)
            time.sleep(args.delay)
    else:
        print("Use --image <path> for single shot, or --loop to cycle all images.")