FROM ubuntu:latest

RUN apt-get update && apt-get install -y \
    sudo

RUN sudo apt install -y python3-pip 
RUN yes | sudo apt install gfortran 
RUN yes | sudo apt install gcc 
RUN yes | sudo apt install git 

RUN yes | sudo apt install curl
RUN yes | sudo apt install wget 

WORKDIR /

RUN mkdir ./home/coqui_dir
COPY ./coqui_dir ./home/coqui_dir


WORKDIR / 

WORKDIR /home/coqui_dir
RUN make install

RUN pip3 install --no-cache-dir -r requirements.txt

WORKDIR / 

RUN sudo apt update
RUN yes | sudo apt install espeak-ng --fix-missing


COPY ./start_coqui.sh /home/coqui_dir
WORKDIR /home/coqui_dir
RUN chmod 555 start_coqui.sh 

EXPOSE 5002

CMD "./start_coqui.sh"

