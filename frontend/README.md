# Frontend scaffold

This directory contains a lightweight full-stack scaffold that connects a React dashboard to a FastAPI gateway for the C++ limit order book engine.

## Structure

- frontend/ — Vite + React client
- python/backend/ — FastAPI application and Pydantic models

## Run locally

1. Install backend dependencies:
   ```bash
   cd python/backend && python3 -m pip install -r requirements.txt
   ```

2. Start the API server from the backend directory:
   ```bash
   cd python/backend && uvicorn app.main:app --reload --host 127.0.0.1 --port 8000
   ```

3. Install frontend dependencies:
   ```bash
   cd frontend && npm install
   ```

4. Start the React dev server:
   ```bash
   cd frontend && npm run dev
   ```

The UI is wired to the FastAPI endpoints at `/orders`, `/backtests`, and `/ws/book`.
