# ─────────────────────────────────────────────────────────────────────────────
# Stage 1 — build the C++ engine into a wheel.
#
# The backend imports `order_book`, the compiled pybind11 extension, so the
# image has to build it. scikit-build-core drives CMake from pyproject.toml,
# which sets ORDER_BOOK_BUILD_TESTS/BENCHMARKS=OFF — so the only FetchContent
# dependency pulled here is pybind11 (network is required at build time).
# ─────────────────────────────────────────────────────────────────────────────
FROM python:3.12-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Everything the build backend reads. pyproject.toml references README.md and
# LICENSE, so both must be present or the build fails on metadata.
COPY pyproject.toml CMakeLists.txt README.md LICENSE /src/
COPY cpp/ /src/cpp/
COPY bindings/ /src/bindings/
COPY python/order_book/ /src/python/order_book/

RUN pip install --no-cache-dir build \
    && pip wheel --no-cache-dir --no-deps . -w /wheels

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2 — runtime. No compiler, no CMake; just the wheel.
# ─────────────────────────────────────────────────────────────────────────────
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

COPY --from=builder /wheels /wheels
COPY python/backend/requirements.txt /tmp/requirements.txt

RUN pip install --no-cache-dir /wheels/*.whl \
    && pip install --no-cache-dir -r /tmp/requirements.txt \
    && rm -rf /wheels

COPY python/backend /app/backend
WORKDIR /app/backend

# Fail the build rather than the first request if the extension didn't land.
RUN python -c "import order_book; print('engine ok:', order_book.TreeOrderBook)"

EXPOSE 8000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
