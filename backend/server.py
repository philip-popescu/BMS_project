"""Run the API with a production WSGI server on port 2026."""

import logging
import os

from waitress import serve

from .api import create_app


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    host = os.getenv("BMS_BIND_HOST", "127.0.0.1")
    print(f"BMS API: http://{host}:2026", flush=True)
    serve(create_app(), host=host, port=2026, threads=4, channel_timeout=10, max_request_body_size=1_048_576)


if __name__ == "__main__":
    main()
