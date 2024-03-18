docker build -t rshiny_image -f Dockerfile_Rshiny.Dockerfile .
docker run --name rshiny_container -p 3838:3838 -it rshiny_image