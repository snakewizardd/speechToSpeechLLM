docker compose down
docker stop kobold_container
docker stop whisper_container
docker stop coqui_container
yes | docker container prune