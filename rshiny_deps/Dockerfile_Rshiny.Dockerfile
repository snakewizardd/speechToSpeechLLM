FROM rocker/shiny-verse:latest

RUN apt-get update && apt-get install -y \
    sudo \
    pandoc \
    pandoc-citeproc \
    libcurl4-gnutls-dev \
    libcairo2-dev \
    libxt-dev \
    libssl-dev \
    libssh2-1-dev \
    default-jdk \ 
    r-cran-rjava \
    libgdal-dev \
    libproj-dev \
    software-properties-common \
    curl



RUN R -e "install.packages('shiny', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('shinybusy', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('httr', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('jsonlite', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('dplyr', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('readr', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('tuneR', repos ='http://cran.rstudio.com/')"
RUN R -e "install.packages('stringr', repos ='http://cran.rstudio.com/')"

RUN yes | sudo apt update
RUN yes | sudo apt install sox

EXPOSE 3838 

WORKDIR /

RUN mkdir ./srv/shiny-server/shiny_functions 
COPY ./shiny_functions ./srv/shiny-server/shiny_functions

COPY shiny.R /srv/shiny-server/

WORKDIR /srv/shiny-server/

CMD ["R", "-e", "shiny::runApp('./shiny.R', port = 3838, host = '0.0.0.0')"]



