docker build -t kobold_image -f Dockerfile_Kobold.Dockerfile .
docker run --name kobold_container -p 5001:5001 -it kobold_image