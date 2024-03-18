docker build -t whisper_image -f Dockerfile_Whisper.Dockerfile .
docker run --name whisper_container -p 8080:8080 -it whisper_image