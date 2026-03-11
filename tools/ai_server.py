#!/usr/bin/env python3
"""
AI Voice Assistant relay server for ESPro smartwatch.

Pipeline:  Raw PCM audio -> Whisper STT -> LLM -> edge-tts TTS -> Raw PCM audio

Setup:
    1.  pip install flask groq edge-tts pydub
    2.  Install ffmpeg: https://ffmpeg.org/download.html  (needed by pydub)
    3.  Get free Groq API key: https://console.groq.com
    4.  Set environment variable:
            Windows:   set GROQ_API_KEY=gsk_...
            Linux:     export GROQ_API_KEY=gsk_...
    5.  Run:  python ai_server.py
    6.  Update AI_SERVER_HOST in hw_config.h with this PC's local IP address.

Binary response format (sent back to ESP32):
    [2 bytes LE: transcript_len] [transcript_utf8]
    [2 bytes LE: response_len]   [response_utf8]
    [remaining bytes: raw PCM 16 kHz 16-bit signed mono]
"""

import os
import io
import struct
import asyncio
import tempfile
import wave
import pathlib

from flask import Flask, request, Response
from groq import Groq
import edge_tts
from pydub import AudioSegment

app = Flask(__name__)

# ── Load .env file if present ───────────────────────────────
_env_path = pathlib.Path(__file__).parent / ".env"
if _env_path.exists():
    for line in _env_path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())

# ── Configuration (override via environment variables) ──────
GROQ_API_KEY = os.environ.get("GROQ_API_KEY", "")
AI_MODEL     = os.environ.get("AI_MODEL", "llama-3.3-70b-versatile")
TTS_VOICE    = os.environ.get("TTS_VOICE", "en-US-AriaNeural")
SAMPLE_RATE  = 16000

groq_client = Groq(api_key=GROQ_API_KEY) if GROQ_API_KEY else None

SYSTEM_PROMPT = (
    "You are ESPro, a helpful, concise AI assistant on a smartwatch. "
    "Keep responses under 40 words — they will be spoken aloud and shown "
    "on a tiny 640x172 pixel screen. Be friendly, direct, and useful."
)


# ── Helpers ─────────────────────────────────────────────────
def pcm_to_wav(pcm: bytes) -> bytes:
    """Wrap raw PCM in a WAV header."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def stt(wav_bytes: bytes) -> str:
    """Speech-to-text using Groq Whisper."""
    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    tmp.write(wav_bytes)
    tmp.close()
    try:
        with open(tmp.name, "rb") as f:
            result = groq_client.audio.transcriptions.create(
                file=("audio.wav", f),
                model="whisper-large-v3-turbo",
                language="en",
            )
        return result.text.strip()
    finally:
        os.unlink(tmp.name)


def llm(prompt: str) -> str:
    """Generate response using Groq LLM."""
    resp = groq_client.chat.completions.create(
        model=AI_MODEL,
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user",   "content": prompt},
        ],
        max_tokens=150,
        temperature=0.7,
    )
    return resp.choices[0].message.content.strip()


async def _tts(text: str) -> bytes:
    """Text-to-speech using edge-tts, returns PCM 16 kHz mono 16-bit."""
    comm = edge_tts.Communicate(text, TTS_VOICE)
    mp3_buf = io.BytesIO()
    async for chunk in comm.stream():
        if chunk["type"] == "audio":
            mp3_buf.write(chunk["data"])
    mp3_buf.seek(0)
    audio = AudioSegment.from_mp3(mp3_buf)
    audio = audio.set_frame_rate(SAMPLE_RATE).set_channels(1).set_sample_width(2)
    return audio.raw_data


def tts(text: str) -> bytes:
    """Synchronous wrapper for async edge-tts."""
    return asyncio.run(_tts(text))


# ── Endpoint ────────────────────────────────────────────────
@app.route("/assist", methods=["POST"])
def assist():
    """Receive raw PCM, return [transcript + response + TTS PCM]."""
    pcm = request.get_data()
    if len(pcm) < 1600:
        return Response(b"Audio too short", status=400)

    print(f"  Received {len(pcm)} bytes of audio")

    # 1. Speech-to-text
    transcript = stt(pcm_to_wav(pcm))
    print(f"  [STT] {transcript}")
    if not transcript:
        transcript = "(silence)"

    # 2. LLM
    answer = llm(transcript)
    print(f"  [LLM] {answer}")

    # 3. Text-to-speech
    audio = tts(answer)
    print(f"  [TTS] {len(audio)} bytes PCM")

    # 4. Pack binary response
    t = transcript.encode("utf-8")[:500]
    r = answer.encode("utf-8")[:1000]
    out = struct.pack("<H", len(t)) + t + struct.pack("<H", len(r)) + r + audio
    return Response(out, content_type="application/octet-stream")


@app.route("/health")
def health():
    return {"status": "ok", "model": AI_MODEL, "voice": TTS_VOICE}


# ── Main ────────────────────────────────────────────────────
if __name__ == "__main__":
    port = int(os.environ.get("SERVER_PORT", "5000"))
    if not GROQ_API_KEY:
        print("WARNING: GROQ_API_KEY not set!")
        print("  Get a free key at https://console.groq.com")
        print("  Then: set GROQ_API_KEY=gsk_...")
    print(f"  Model : {AI_MODEL}")
    print(f"  Voice : {TTS_VOICE}")
    print(f"  Listening on 0.0.0.0:{port}")
    app.run(host="0.0.0.0", port=port, debug=False)
