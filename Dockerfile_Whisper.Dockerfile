FROM ubuntu:latest

RUN apt-get update && apt-get install -y \
    sudo

RUN sudo apt install -y python3-pip 
RUN yes | sudo apt install gfortran 
RUN yes | sudo apt install gcc 
RUN yes | sudo apt install git 

RUN yes | sudo apt install curl
RUN yes | sudo apt install wget 

RUN mkdir ./home/whisper_dir
COPY ./whisper_dir ./home/whisper_dir

WORKDIR /home/whisper_dir
RUN make

WORKDIR / 
WORKDIR /home/whisper_dir/models

RUN wget --no-check-certificate -O ggml-medium.en.bin "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin?download=true"


WORKDIR / 

COPY ./start_whisper.sh /home/whisper_dir
WORKDIR /home/whisper_dir
RUN chmod 555 start_whisper.sh 


EXPOSE 8080

WORKDIR /home/whisper_dir

CMD "./start_whisper.sh"