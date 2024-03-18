docker build -t coqui_image -f Dockerfile_Coqui.Dockerfile .
docker run --name coqui_container -p 5002:5002 -it coqui_image