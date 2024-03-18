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

RUN mkdir ./home/koboldcpp_dir
COPY ./koboldcpp_dir ./home/koboldcpp_dir


WORKDIR /home/koboldcpp_dir/models 

RUN wget --no-check-certificate -O neuralbeagle14-7b.Q4_K_M.gguf "https://huggingface.co/TheBloke/NeuralBeagle14-7B-GGUF/resolve/main/neuralbeagle14-7b.Q4_K_M.gguf?download=true"

RUN wget --no-check-certificate -O mistral-7b-mmproj-v1.5-Q4_1.gguf "https://huggingface.co/koboldcpp/mmproj/resolve/main/mistral-7b-mmproj-v1.5-Q4_1.gguf?download=true"


WORKDIR / 

WORKDIR /home/koboldcpp_dir
RUN make

WORKDIR / 

COPY ./start_kobold.sh /home/koboldcpp_dir
WORKDIR /home/koboldcpp_dir
RUN chmod 555 start_kobold.sh 

EXPOSE 5001

CMD "./start_kobold.sh"

