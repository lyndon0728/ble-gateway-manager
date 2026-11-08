"""Isolated text-only PDF worker. No document actions or external links run."""
import sys
from pypdf import PdfReader

MAX_PAGES = 200
MAX_BYTES = 2 * 1024 * 1024

def main():
    reader = PdfReader(sys.argv[1], strict=False)
    if reader.is_encrypted:
        raise ValueError("DOCUMENT_ENCRYPTED")
    if len(reader.pages) > MAX_PAGES:
        raise ValueError("DOCUMENT_PAGE_LIMIT")
    parts = []
    count = 0
    for page in reader.pages:
        text = page.extract_text() or ""
        count += len(text.encode("utf-8")) + 1
        if count > MAX_BYTES:
            raise ValueError("DOCUMENT_TEXT_LIMIT")
        parts.append(text)
    sys.stdout.buffer.write(("\f".join(parts) + "\f").encode("utf-8"))

if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        code = str(error) if str(error) in {
            "DOCUMENT_ENCRYPTED", "DOCUMENT_PAGE_LIMIT", "DOCUMENT_TEXT_LIMIT"
        } else "DOCUMENT_EXTRACTION_FAILED"
        sys.stderr.write(code)
        sys.exit(1)
