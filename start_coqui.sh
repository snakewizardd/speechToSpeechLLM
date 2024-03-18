#!/bin/bash

retry_count=5
retry_delay=5

for ((i=1; i<=$retry_count; i++)); do
    echo "Attempt $i to download model"
    python3 TTS/server/server.py --model_name tts_models/en/vctk/vits && break
    echo "Download attempt failed, retrying in $retry_delay seconds..."
    sleep $retry_delay
done
