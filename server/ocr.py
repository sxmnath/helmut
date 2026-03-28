import os
import re
import requests
import base64

OCR_API_KEY     = os.getenv("OCR_SPACE_API_KEY", "helloworld")
OCR_API_URL     = "https://api.ocr.space/parse/image"
PLATE_PATTERN   = re.compile(
    r'[A-Z]{2}[\s\-]?[0-9]{2}[\s\-]?[A-Z]{1,2}[\s\-]?[0-9]{4}'
)

def extract_plate_from_bytes(image_bytes: bytes) -> str:
    try:
        # Encode image as base64 and send to OCR.space
        b64 = base64.b64encode(image_bytes).decode('utf-8')
        payload = {
            "apikey":       OCR_API_KEY,
            "base64Image":  f"data:image/jpeg;base64,{b64}",
            "language":     "eng",
            "isOverlayRequired": False,
            "detectOrientation": True,
            "scale":        True,
            "OCREngine":    2,   # Engine 2 better for plates
        }

        resp = requests.post(OCR_API_URL, data=payload, timeout=15)
        resp.raise_for_status()
        result = resp.json()

        if result.get("IsErroredOnProcessing"):
            print(f"OCR.space error: {result.get('ErrorMessage')}")
            return "OCR_ERROR"

        parsed = result.get("ParsedResults", [])
        if not parsed:
            return "UNREADABLE"

        full_text = parsed[0].get("ParsedText", "").upper().replace("\n", " ").replace("\r", "")
        print(f"OCR.space raw: {full_text[:80]}")

        # Try Indian plate regex first
        match = PLATE_PATTERN.search(full_text)
        if match:
            plate = re.sub(r'[\s\-]', '', match.group())
            return plate

        # Fallback: return first 20 chars of whatever was read
        clean = re.sub(r'[^A-Z0-9\s]', '', full_text).strip()
        return clean[:20] or "UNREADABLE"

    except requests.exceptions.Timeout:
        return "OCR_TIMEOUT"
    except Exception as e:
        print(f"OCR exception: {e}")
        return "OCR_ERROR"