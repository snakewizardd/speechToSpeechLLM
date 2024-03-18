#!/bin/bash
python3 koboldcpp.py  \
--model ./models/neuralbeagle14-7b.Q4_K_M.gguf \
--launch \
--threads 6 \
--contextsize 6144 \
--smartcontext \
--debugmode \
--blasbatchsize 64 \
--mmproj ./models/mistral-7b-mmproj-v1.5-Q4_1.gguf